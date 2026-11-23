#include "backend.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <set>
#include <utility>

namespace huxerui::bluetooth::detail {

std::unique_ptr<PeripheralBackend> CreateLinuxPeripheral(
    BackendEvents events, std::function<std::shared_ptr<GDBusProxy>(const char*)> manager);

namespace {

using Code = BluetoothErrorCode;
constexpr char bluez_name[] = "org.bluez";
constexpr char adapter_interface[] = "org.bluez.Adapter1";
constexpr char device_interface[] = "org.bluez.Device1";
constexpr char service_interface[] = "org.bluez.GattService1";
constexpr char characteristic_interface[] = "org.bluez.GattCharacteristic1";
constexpr char descriptor_interface[] = "org.bluez.GattDescriptor1";

template <class T> std::shared_ptr<T> OwnObject(T* object) {
  return object ? std::shared_ptr<T>(object, [](T* value) { g_object_unref(value); }) : nullptr;
}

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;

Variant OwnVariant(GVariant* value) {
  return Variant(value, g_variant_unref);
}

Variant Property(GDBusProxy* proxy, const char* name) {
  return OwnVariant(g_dbus_proxy_get_cached_property(proxy, name));
}

std::optional<std::string> Text(GVariant* value) {
  return value ? std::optional(std::string(g_variant_get_string(value, nullptr))) : std::nullopt;
}

std::string String(GDBusProxy* proxy, const char* name) {
  return Text(Property(proxy, name).get()).value_or("");
}

bool Flag(GDBusProxy* proxy, const char* name) {
  const auto value = Property(proxy, name);
  return value && g_variant_get_boolean(value.get());
}

std::vector<std::string> Strings(GVariant* array) {
  std::vector<std::string> strings;
  if (array) {
    GVariantIter iter;
    g_variant_iter_init(&iter, array);
    const char* value = nullptr;
    while (g_variant_iter_next(&iter, "&s", &value))
      strings.emplace_back(value);
  }
  return strings;
}

Bytes ReadBytes(GVariant* value) {
  gsize count = 0;
  const auto* bytes = static_cast<const std::byte*>(g_variant_get_fixed_array(value, &count, 1));
  return count ? Bytes(bytes, bytes + count) : Bytes{};
}

BluetoothError BusError(const GError& error) {
  std::unique_ptr<char, decltype(&g_free)> remote(g_dbus_error_get_remote_error(&error), g_free);
  std::string domain = remote ? remote.get() : g_quark_to_string(error.domain);
  Code code = Code::PlatformFailure;
  if (domain.ends_with(".NotAuthorized") || domain.ends_with(".AccessDenied"))
    code = Code::PermissionDenied;
  else if (domain.ends_with(".NotReady"))
    code = Code::Unavailable;
  else if (domain.ends_with(".NotSupported") || domain.ends_with(".UnknownMethod"))
    code = Code::Unsupported;
  else if (domain.ends_with(".NotPermitted"))
    code = Code::UnsupportedProperty;
  else if (domain.ends_with(".AuthenticationCanceled"))
    code = Code::Canceled;
  else if (domain.ends_with(".AuthenticationFailed") || domain.ends_with(".AuthenticationRejected"))
    code = Code::AuthenticationFailed;
  else if (domain.ends_with(".AuthenticationTimeout") || domain.ends_with(".NoReply") ||
           g_error_matches(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    code = Code::TimedOut;
  else if (domain.ends_with(".InvalidValueLength"))
    code = Code::PayloadTooLarge;
  else if (domain.ends_with(".NotConnected"))
    code = Code::Disconnected;
  else if (domain.ends_with(".InProgress"))
    code = Code::Busy;
  else if (domain.ends_with(".UnknownObject"))
    code = Code::StaleAttribute;
  else if (domain.ends_with(".ServiceUnknown") || domain.ends_with(".NameHasNoOwner"))
    code = Code::Unavailable;
  else if (g_error_matches(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    code = Code::Canceled;
  return Error(code, error.message, std::move(domain), error.code);
}

using BusReply = std::function<void(Variant, std::optional<BluetoothError>)>;

void Call(GDBusProxy* proxy, const char* method, GVariant* arguments, BusReply receive, GCancellable* cancel = nullptr) {
  g_dbus_proxy_call(proxy, method, arguments, G_DBUS_CALL_FLAGS_NONE, -1, cancel,
                    [](GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<BusReply> reply(static_cast<BusReply*>(data));
    GError* error = nullptr;
    auto value = OwnVariant(g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error));
    if (error) {
      const std::unique_ptr<GError, decltype(&g_error_free)> failure(error, g_error_free);
      (*reply)(std::move(value), BusError(*failure));
    } else {
      (*reply)(std::move(value), {});
    }
  }, new BusReply(std::move(receive)));
}

GVariant* EmptyOptions() {
  return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
}

struct Link {
  std::string path;
  std::shared_ptr<GDBusProxy> device;
  Completion connecting;
  bool connected = false;
  bool owns_connection = false;
  bool waiting_for_connect_reply = true;
  std::map<Token, std::shared_ptr<GDBusProxy>> attributes;
  std::set<Token> notifications;
};

constexpr char profile_manager_interface[] = "org.bluez.ProfileManager1";
constexpr char profile_interface[] = "org.bluez.Profile1";
constexpr char profile_xml[] = R"(
<node><interface name="org.bluez.Profile1">
  <method name="Release"/>
  <method name="NewConnection">
    <arg type="o" direction="in"/>
    <arg type="h" direction="in"/>
    <arg type="a{sv}" direction="in"/>
  </method>
  <method name="RequestDisconnection"><arg type="o" direction="in"/></method>
</interface></node>)";

BluetoothError SocketError(int error, Code fallback) {
  const auto code = error == EACCES || error == EPERM ? Code::PermissionDenied
                  : error == ETIMEDOUT ? Code::TimedOut
                  : error == EAFNOSUPPORT || error == EPROTONOSUPPORT ? Code::Unsupported
                  : fallback;
  return Error(code, std::strerror(error), "errno", error);
}

bool ParseAddress(const std::string& value, bdaddr_t& address) {
  unsigned char bytes[6];
  int consumed = 0;
  if (std::sscanf(value.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx%n",
                  &bytes[5], &bytes[4], &bytes[3], &bytes[2], &bytes[1], &bytes[0], &consumed) != 6 ||
      static_cast<std::size_t>(consumed) != value.size()) {
    return false;
  }
  std::memcpy(address.b, bytes, sizeof(bytes));
  return true;
}

struct SocketChannel {
  explicit SocketChannel(std::string device, Completion reply)
      : device(std::move(device)), opening(std::move(reply)) {}

  ~SocketChannel() {
    if (read_source)
      g_source_remove(read_source);
    if (write_source)
      g_source_remove(write_source);
    if (fd >= 0)
      close(fd);
    if (object)
      g_dbus_connection_unregister_object(bus.get(), object);
  }

  std::string device;
  std::string uuid;
  std::shared_ptr<GDBusProxy> profile_manager;
  std::shared_ptr<GDBusConnection> bus;
  std::string object_path;
  guint object = 0;
  int fd = -1;
  guint read_source = 0;
  guint write_source = 0;
  std::uint16_t transmit_limit = 0;
  bool packet = false;
  bool registered = false;
  bool connecting = true;
  bool profile_connected = false;
  bool retiring = false;
  Completion opening;
  Completion reading;
  Completion writing;
  Bytes output;
  std::size_t written = 0;
};

struct State : std::enable_shared_from_this<State> {
  explicit State(BackendEvents receive) : events(std::move(receive)) {}

  ~State() {
    if (manager) {
      g_signal_handlers_disconnect_by_data(manager.get(), this);
      if (scan_proxy)
        Call(scan_proxy.get(), "StopDiscovery", nullptr, [](Variant, auto) {});
    }
    while (!channels.empty()) {
      auto channel = channels.extract(channels.begin());
      if (channel.mapped()->registered) {
        Call(channel.mapped()->profile_manager.get(), "UnregisterProfile",
             g_variant_new("(o)", channel.mapped()->object_path.c_str()), [](Variant, auto) {});
      }
    }
    for (const auto& [id, link] : links) {
      CleanupNotifications(link);
      if (link.owns_connection)
        Call(link.device.get(), "Disconnect", nullptr, [](Variant, auto) {});
    }
    for (const auto& [path, device] : held_connections)
      Call(device.get(), "Disconnect", nullptr, [](Variant, auto) {});
  }

  void Initialize() {
    g_dbus_object_manager_client_new_for_bus(
        G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, bluez_name, "/", nullptr, nullptr,
        nullptr, nullptr, [](GObject*, GAsyncResult* result, gpointer data) {
      const std::unique_ptr<std::weak_ptr<State>> weak(static_cast<std::weak_ptr<State>*>(data));
      GError* error = nullptr;
      auto manager = OwnObject(g_dbus_object_manager_client_new_for_bus_finish(result, &error));
      if (error)
        g_error_free(error);
      if (auto state = weak->lock())
        state->Attach(std::move(manager));
    }, new std::weak_ptr<State>(weak_from_this()));
  }

  void Attach(std::shared_ptr<GDBusObjectManager> object_manager) {
    manager = std::move(object_manager);
    initializing = false;
    if (manager) {
      g_signal_connect(manager.get(), "notify::name-owner", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        static_cast<State*>(data)->RefreshAdapter();
      }), this);
      g_signal_connect(manager.get(), "object-added",
                       G_CALLBACK(+[](GDBusObjectManager*, GDBusObject* object, gpointer data) {
        static_cast<State*>(data)->Added(object);
      }), this);
      g_signal_connect(manager.get(), "object-removed",
                       G_CALLBACK(+[](GDBusObjectManager*, GDBusObject* object, gpointer data) {
        static_cast<State*>(data)->Removed(object);
      }), this);
      g_signal_connect(manager.get(), "interface-proxy-properties-changed",
                       G_CALLBACK(+[](GDBusObjectManagerClient*, GDBusObjectProxy*, GDBusProxy* proxy,
                                      GVariant* changed, const gchar* const*, gpointer data) {
        static_cast<State*>(data)->Changed(proxy, changed);
      }), this);
    }
    RefreshAdapter();
    auto ready = std::exchange(queued, {});
    for (auto& [id, pair] : ready)
      Invoke(std::move(pair.first), std::move(pair.second));
  }

  std::vector<std::shared_ptr<GDBusProxy>> Objects(const char* interface) const {
    std::vector<std::shared_ptr<GDBusProxy>> rows;
    if (!manager)
      return rows;
    GList* objects = g_dbus_object_manager_get_objects(manager.get());
    for (auto* item = objects; item; item = item->next) {
      auto* object = G_DBUS_OBJECT(item->data);
      if (auto* proxy = g_dbus_object_get_interface(object, interface))
        rows.emplace_back(OwnObject(G_DBUS_PROXY(proxy)));
      g_object_unref(object);
    }
    g_list_free(objects);
    return rows;
  }

  std::shared_ptr<GDBusProxy> Proxy(const std::string& path, const char* interface) const {
    if (!manager || !g_variant_is_object_path(path.c_str()))
      return {};
    return OwnObject(
        G_DBUS_PROXY(g_dbus_object_manager_get_interface(manager.get(), path.c_str(), interface)));
  }

  void RefreshAdapter() {
    auto adapters = Objects(adapter_interface);
    if (adapter) {
      const std::string path = g_dbus_proxy_get_object_path(adapter.get());
      adapter = Proxy(path, adapter_interface);
    }
    if (!adapter && !adapters.empty()) {
      adapter = *std::min_element(adapters.begin(), adapters.end(), [](const auto& left, const auto& right) {
        return std::string_view(g_dbus_proxy_get_object_path(left.get())) < g_dbus_proxy_get_object_path(right.get());
      });
    }
    const auto next_status = adapter ? Flag(adapter.get(), "Powered") ? AdapterStatus::PoweredOn : AdapterStatus::PoweredOff
                                     : AdapterStatus::Unavailable;
    if (status != next_status) {
      status = next_status;
      events(status);
    }
  }

  bool OnAdapter(GDBusProxy* device) const {
    return adapter && String(device, "Adapter") == g_dbus_proxy_get_object_path(adapter.get());
  }

  FoundDevice Describe(GDBusProxy* device) const {
    auto name = Text(Property(device, "Name").get());
    if (!name)
      name = Text(Property(device, "Alias").get());
    return {g_dbus_proxy_get_object_path(device), std::move(name)};
  }

  void Report(GDBusProxy* device) {
    if (!scan || !OnAdapter(device))
      return;
    const auto rssi = Property(device, "RSSI");
    Advertisement advertisement;
    advertisement.local_name = Text(Property(device, "Name").get());
    advertisement.service_uuids = Strings(Property(device, "UUIDs").get());
    if (rssi)
      advertisement.rssi_dbm = g_variant_get_int16(rssi.get());
    if (const auto data = Property(device, "ManufacturerData")) {
      GVariantIter iter;
      g_variant_iter_init(&iter, data.get());
      guint16 company = 0;
      GVariant* value = nullptr;
      while (g_variant_iter_next(&iter, "{qv}", &company, &value)) {
        const auto entry = OwnVariant(value);
        advertisement.manufacturer_data.emplace(company, ReadBytes(value));
      }
    }
    if (const auto data = Property(device, "ServiceData")) {
      GVariantIter iter;
      g_variant_iter_init(&iter, data.get());
      const char* uuid = nullptr;
      GVariant* value = nullptr;
      while (g_variant_iter_next(&iter, "{&sv}", &uuid, &value)) {
        const auto entry = OwnVariant(value);
        advertisement.service_data.emplace(NormalizeUuid(uuid).value_or(uuid), ReadBytes(value));
      }
    }
    events(ScanReport{scan, Describe(device), std::move(advertisement)});
  }

  void Added(GDBusObject* object) {
    if (auto adapter_proxy = OwnObject(g_dbus_object_get_interface(object, adapter_interface))) {
      RefreshAdapter();
    } else if (auto device = OwnObject(g_dbus_object_get_interface(object, device_interface))) {
      Report(G_DBUS_PROXY(device.get()));
    }
  }

  void Removed(GDBusObject* object) {
    const std::string path = g_dbus_object_get_object_path(object);
    if (adapter && path == g_dbus_proxy_get_object_path(adapter.get()))
      RefreshAdapter();
    LoseChannels(path, "BlueZ removed the connected device");
    for (auto& [id, link] : links) {
      if (link.path == path) {
        events(LinkLost{id, Error(Code::Disconnected, "BlueZ removed the connected device")});
      } else if (path.starts_with(link.path + "/service") && !link.attributes.empty() &&
                 path.find("/char") == std::string::npos) {
        events(ServicesChanged{id});
      }
    }
  }

  void Changed(GDBusProxy* proxy, GVariant* changed) {
    const std::string_view type = g_dbus_proxy_get_interface_name(proxy);
    const std::string path = g_dbus_proxy_get_object_path(proxy);
    if (type == adapter_interface) {
      RefreshAdapter();
    } else if (type == device_interface) {
      const auto connected = OwnVariant(g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN));
      if (connected && !g_variant_get_boolean(connected.get()))
        LoseChannels(path, "BlueZ reported a Bluetooth disconnect");
      if (OwnVariant(g_variant_lookup_value(changed, "RSSI", nullptr)) ||
          OwnVariant(g_variant_lookup_value(changed, "ManufacturerData", nullptr)) ||
          OwnVariant(g_variant_lookup_value(changed, "ServiceData", nullptr)) ||
          OwnVariant(g_variant_lookup_value(changed, "UUIDs", nullptr))) {
        Report(proxy);
      }
      for (auto& [id, link] : links) {
        if (link.path != path)
          continue;
        if (!link.connected) {
          TryConnected(link);
        } else if (!Flag(proxy, "Connected")) {
          events(LinkLost{id, Error(Code::Disconnected, "BlueZ reported a GATT disconnect")});
        } else if (OwnVariant(g_variant_lookup_value(changed, "ServicesResolved", nullptr)) &&
                   !Flag(proxy, "ServicesResolved")) {
          events(ServicesChanged{id});
        }
      }
    } else if (type == characteristic_interface) {
      const auto value = OwnVariant(g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay")));
      if (!value)
        return;
      for (auto& [id, link] : links) {
        for (auto attribute : link.notifications) {
          if (const auto found = link.attributes.find(attribute);
              found != link.attributes.end() && path == g_dbus_proxy_get_object_path(found->second.get())) {
            events(ValueChanged{id, attribute, ReadBytes(value.get())});
          }
        }
      }
    }
  }

  void LoseChannels(const std::string& path, const char* message) {
    for (const auto& [id, channel] : channels)
      if (channel->device == path && !channel->connecting)
        events(ChannelLost{id, Error(Code::Disconnected, message)});
  }

  Cancellation Defer(Request request, Completion reply) {
    const auto id = next++;
    queued.emplace(id, std::pair{std::move(request), std::move(reply)});
    return [weak = weak_from_this(), id] {
      if (auto state = weak.lock())
        state->queued.erase(id);
    };
  }

  Cancellation Invoke(Request request, Completion reply) {
    if (initializing || (request.operation == Operation::Connect && retiring_devices.contains(request.device)))
      return Defer(std::move(request), std::move(reply));
    if (!adapter) {
      reply(Fail(Code::Unavailable, "The BlueZ adapter or system service is unavailable"));
      return {};
    }
    switch (request.operation) {
      case Operation::ListRfcomm: {
        std::vector<FoundDevice> candidates;
        for (auto device : Objects(device_interface)) {
          if (OnAdapter(device.get()) && Flag(device.get(), "Paired") &&
              String(device.get(), "AddressType") != "random")
            candidates.push_back(Describe(device.get()));
        }
        reply(Ok(std::move(candidates)));
        return {};
      }
      case Operation::OpenRfcomm:
      case Operation::OpenL2cap:
        OpenChannel(std::move(request), std::move(reply));
        return {};
      case Operation::ChannelRead:
      case Operation::ChannelWrite:
        SocketIo(std::move(request), std::move(reply));
        return {};
      case Operation::Scan:
        StartScan(std::move(request), std::move(reply));
        return {};
      case Operation::Restore:
      case Operation::Bond:
      case Operation::Pair:
      case Operation::Connect:
        return DeviceRequest(std::move(request), std::move(reply));
      case Operation::Discover:
      case Operation::Read:
      case Operation::Write:
      case Operation::Notify:
        return AttributeRequest(std::move(request), std::move(reply));
      default:
        reply(Fail(Code::Unsupported, "BlueZ does not support this Bluetooth operation"));
        return {};
    }
  }

  void OpenChannel(Request request, Completion reply) {
    auto device = Proxy(request.device, device_interface);
    if (!device || !OnAdapter(device.get())) {
      reply(Fail(Code::DeviceUnavailable, "BlueZ no longer recognizes this Bluetooth peer"));
      return;
    }
    if (retiring_devices.contains(request.device)) {
      reply(Fail(Code::Busy, "The BlueZ link is still closing"));
      return;
    }
    for (const auto& [id, channel] : channels) {
      if (channel->device == request.device && channel->connecting) {
        reply(Fail(Code::Busy, "A Bluetooth channel is already opening for the peer"));
        return;
      }
    }
    const auto id = request.link;
    auto channel = std::make_unique<SocketChannel>(request.device, std::move(reply));
    channel->packet = request.operation == Operation::OpenL2cap;
    auto& opened = *channels.emplace(id, std::move(channel)).first->second;
    if (opened.packet)
      ConnectL2cap(id, opened, device.get(), request.psm);
    else
      RegisterRfcomm(id, opened, std::move(device), std::move(request.service_uuid));
  }

  void FinishOpen(Token id, Reply result) {
    auto& channel = *channels.at(id);
    channel.connecting = false;
    if (auto reply = std::exchange(channel.opening, {}))
      reply(std::move(result));
  }

  void CompleteL2cap(Token id, SocketChannel& channel) {
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(channel.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
      error = errno;
    size = sizeof(channel.transmit_limit);
    if (!error && getsockopt(channel.fd, SOL_BLUETOOTH, BT_SNDMTU, &channel.transmit_limit, &size) < 0)
      error = errno;
    if (error || !channel.transmit_limit) {
      FinishOpen(id, Reply::Failure(SocketError(error ? error : EPROTO, Code::DeviceUnavailable)));
      return;
    }
    FinishOpen(id, Ok());
  }

  void ConnectL2cap(Token id, SocketChannel& channel, GDBusProxy* device, std::uint16_t psm) {
    sockaddr_l2 source{};
    source.l2_family = AF_BLUETOOTH;
    source.l2_bdaddr_type = String(adapter.get(), "AddressType") == "random" ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
    sockaddr_l2 destination{};
    destination.l2_family = AF_BLUETOOTH;
    destination.l2_psm = htobs(psm);
    const auto address_type = String(device, "AddressType");
    destination.l2_bdaddr_type = address_type == "random" ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
    if ((address_type != "random" && address_type != "public") ||
        !ParseAddress(String(adapter.get(), "Address"), source.l2_bdaddr) ||
        !ParseAddress(String(device, "Address"), destination.l2_bdaddr)) {
      FinishOpen(id, Fail(Code::DeviceUnavailable, "The BlueZ LE address or address type is unavailable"));
      return;
    }
    channel.fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
    bt_security security{BT_SECURITY_MEDIUM, 0};
    if (channel.fd < 0 ||
        setsockopt(channel.fd, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof(security)) < 0 ||
        bind(channel.fd, reinterpret_cast<sockaddr*>(&source), sizeof(source)) < 0) {
      FinishOpen(id, Reply::Failure(SocketError(errno, Code::PlatformFailure)));
      return;
    }
    if (connect(channel.fd, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0) {
      CompleteL2cap(id, channel);
    } else if (errno == EINPROGRESS || errno == EAGAIN) {
      WatchSocket(id, channel, false);
    } else {
      FinishOpen(id, Reply::Failure(SocketError(errno, Code::DeviceUnavailable)));
    }
  }

  void RegisterRfcomm(Token id, SocketChannel& channel, std::shared_ptr<GDBusProxy> device, std::string uuid) {
    auto manager = Proxy("/org/bluez", profile_manager_interface);
    if (!manager) {
      FinishOpen(id, Fail(Code::Unsupported, "BlueZ ProfileManager1 is unavailable"));
      return;
    }
    channel.profile_manager = manager;
    channel.uuid = std::move(uuid);
    channel.bus = OwnObject(G_DBUS_CONNECTION(g_object_ref(g_dbus_proxy_get_connection(manager.get()))));
    channel.object_path = "/org/huxerui/bluetooth/profile_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                          "_" + std::to_string(id);
    const auto info = std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)>(
        g_dbus_node_info_new_for_xml(profile_xml, nullptr), g_dbus_node_info_unref);
    struct ProfileTarget {
      std::weak_ptr<State> state;
      Token id;
    };
    static const GDBusInterfaceVTable methods{
        .method_call = [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* method,
                          GVariant* arguments, GDBusMethodInvocation* invocation, gpointer data) {
          auto& target = *static_cast<ProfileTarget*>(data);
          if (auto state = target.state.lock())
            state->ProfileMethod(target.id, sender, method, arguments, invocation);
          else
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Profile closed");
        }};
    auto target = std::make_unique<ProfileTarget>(ProfileTarget{weak_from_this(), id});
    channel.object = g_dbus_connection_register_object(channel.bus.get(), channel.object_path.c_str(),
                                                       info->interfaces[0], &methods, target.get(),
                                                       [](gpointer data) { delete static_cast<ProfileTarget*>(data); },
                                                       nullptr);
    if (!channel.object) {
      FinishOpen(id, Fail(Code::PlatformFailure, "The RFCOMM profile could not be exported"));
      return;
    }
    target.release();
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("client"));
    g_variant_builder_add(&options, "{sv}", "AutoConnect", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
    Call(manager.get(), "RegisterProfile",
         g_variant_new("(osa{sv})", channel.object_path.c_str(), channel.uuid.c_str(), &options),
         [weak = weak_from_this(), id, manager, path = channel.object_path,
          device = std::move(device)](Variant, auto error) {
      if (auto state = weak.lock()) {
        state->ProfileRegistered(id, device.get(), std::move(error));
      } else if (!error) {
        Call(manager.get(), "UnregisterProfile", g_variant_new("(o)", path.c_str()), [](Variant, auto) {});
      }
    });
  }

  void ProfileRegistered(Token id, GDBusProxy* device, std::optional<BluetoothError> error) {
    const auto found = channels.find(id);
    if (found == channels.end())
      return;
    auto& channel = *found->second;
    if (error) {
      if (error->platform_domain.ends_with(".AlreadyExists"))
        error->code = Code::Busy;
      FinishOpen(id, Reply::Failure(std::move(*error)));
      if (channel.retiring)
        RemoveChannel(id);
      return;
    }
    channel.registered = true;
    if (channel.retiring) {
      RemoveChannel(id);
      return;
    }
    Call(device, "ConnectProfile", g_variant_new("(s)", channel.uuid.c_str()),
         [weak = weak_from_this(), id](Variant, auto error) {
      if (auto state = weak.lock(); state && state->channels.contains(id)) {
        auto& channel = *state->channels.at(id);
        if (error) {
          if (error->platform_domain.ends_with(".NotAvailable"))
            error->code = Code::DeviceUnavailable;
          state->FinishOpen(id, Reply::Failure(std::move(*error)));
        } else {
          channel.profile_connected = true;
          if (channel.fd >= 0)
            state->FinishOpen(id, Ok());
        }
      }
    });
  }

  void ProfileMethod(Token id, const char* sender, std::string_view method, GVariant* arguments,
                     GDBusMethodInvocation* invocation) {
    const auto found = channels.find(id);
    if (found == channels.end()) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Profile closed");
      return;
    }
    auto& channel = *found->second;
    const std::unique_ptr<char, decltype(&g_free)> owner(
        g_dbus_proxy_get_name_owner(channel.profile_manager.get()), g_free);
    const char* device = nullptr;
    if (method != "Release")
      g_variant_get_child(arguments, 0, "&o", &device);
    if (!owner || std::string_view(owner.get()) != sender || (device && channel.device != device)) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Unexpected profile request");
      return;
    }
    if (method == "NewConnection") {
      gint32 handle = -1;
      g_variant_get_child(arguments, 1, "h", &handle);
      auto* message = g_dbus_method_invocation_get_message(invocation);
      auto* list = g_dbus_message_get_unix_fd_list(message);
      if (!channel.retiring && channel.connecting && channel.fd < 0 && list)
        channel.fd = g_unix_fd_list_get(list, handle, nullptr);
      else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Unexpected RFCOMM socket");
        return;
      }
      if (channel.fd < 0 || fcntl(channel.fd, F_SETFL, O_NONBLOCK) < 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Invalid RFCOMM socket");
        FinishOpen(id, Fail(Code::PlatformFailure, "The BlueZ RFCOMM socket is unavailable"));
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      if (channel.profile_connected)
        FinishOpen(id, Ok());
      return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (method == "Release")
      channel.registered = false;
    auto error = Error(Code::Disconnected, "BlueZ closed the RFCOMM profile");
    if (channel.connecting)
      FinishOpen(id, Reply::Failure(std::move(error)));
    else
      events(ChannelLost{id, std::move(error)});
  }

  void WatchSocket(Token id, SocketChannel& channel, bool read) {
    struct Watch {
      std::weak_ptr<State> state;
      Token id;
      bool read;
    };
    auto assignment = g_unix_fd_add_full(G_PRIORITY_DEFAULT, channel.fd,
        static_cast<GIOCondition>((read ? G_IO_IN : G_IO_OUT) | G_IO_HUP | G_IO_ERR),
        [](gint, GIOCondition condition, gpointer data) -> gboolean {
          const auto& watch = *static_cast<Watch*>(data);
          if (auto state = watch.state.lock(); state && state->channels.contains(watch.id)) {
            auto& channel = *state->channels.at(watch.id);
            (watch.read ? channel.read_source : channel.write_source) = 0;
            if (channel.connecting)
              state->CompleteL2cap(watch.id, channel);
            else if (watch.read)
              state->Receive(watch.id, channel, condition);
            else
              state->Send(watch.id, channel);
          }
          return G_SOURCE_REMOVE;
        }, new Watch{weak_from_this(), id, read}, [](gpointer data) { delete static_cast<Watch*>(data); });
    (read ? channel.read_source : channel.write_source) = assignment;
  }

  void Receive(Token id, SocketChannel& channel, GIOCondition condition) {
    Bytes value(channel_read_size);
    for (;;) {
      const auto size = recv(channel.fd, value.data(), value.size(), channel.packet ? MSG_TRUNC : 0);
      if (size < 0 && errno == EINTR)
        continue;
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (condition & (G_IO_HUP | G_IO_ERR)) {
          std::exchange(channel.reading, {})(Fail(Code::Disconnected, "The Bluetooth socket closed"));
        } else {
          WatchSocket(id, channel, true);
        }
        return;
      }
      if (size < 0) {
        std::exchange(channel.reading, {})(Reply::Failure(SocketError(errno, Code::Disconnected)));
      } else if (static_cast<std::size_t>(size) > value.size()) {
        std::exchange(channel.reading, {})(Fail(Code::PlatformFailure, "The LE L2CAP SDU exceeded the receive limit"));
      } else {
        if (size == 0 && channel.packet && !(condition & (G_IO_HUP | G_IO_ERR))) {
          WatchSocket(id, channel, true);
          return;
        }
        if (size == 0 && channel.packet) {
          std::exchange(channel.reading, {})(Fail(Code::Disconnected, "The LE L2CAP socket closed"));
        } else {
          value.resize(size);
          std::exchange(channel.reading, {})(Ok(std::move(value)));
        }
      }
      return;
    }
  }

  void Send(Token id, SocketChannel& channel) {
    while (channel.written < channel.output.size()) {
      const auto remaining = channel.output.size() - channel.written;
      const auto count = channel.packet ? std::min<std::size_t>(remaining, channel.transmit_limit) : remaining;
      const auto written = send(channel.fd, channel.output.data() + channel.written, count, MSG_NOSIGNAL);
      if (written < 0 && errno == EINTR)
        continue;
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        WatchSocket(id, channel, false);
        return;
      }
      if (written <= 0 || (channel.packet && static_cast<std::size_t>(written) != count)) {
        std::exchange(channel.writing, {})(Reply::Failure(SocketError(written < 0 ? errno : EIO, Code::Disconnected)));
        return;
      }
      channel.written += written;
    }
    channel.output = {};
    std::exchange(channel.writing, {})(Ok());
  }

  void SocketIo(Request request, Completion reply) {
    const auto id = request.link;
    const auto found = channels.find(id);
    if (found == channels.end() || found->second->connecting || found->second->fd < 0) {
      reply(Fail(Code::Disconnected, "The Bluetooth socket is closed"));
      return;
    }
    auto& channel = *found->second;
    if (request.operation == Operation::ChannelRead) {
      channel.reading = std::move(reply);
      WatchSocket(id, channel, true);
    } else {
      channel.writing = std::move(reply);
      channel.output = std::move(request.value);
      channel.written = 0;
      Send(id, channel);
    }
  }

  void CloseChannel(Token id) {
    std::erase_if(queued, [id](const auto& item) { return item.second.first.link == id; });
    const auto found = channels.find(id);
    if (found == channels.end())
      return;
    auto& channel = *found->second;
    if (!channel.packet && channel.object && !channel.registered && channel.connecting) {
      channel.retiring = true;
      channel.opening = {};
      return;
    }
    RemoveChannel(id);
  }

  void RemoveChannel(Token id) {
    auto entry = channels.extract(id);
    const auto& channel = *entry.mapped();
    if (channel.registered) {
      Call(channel.profile_manager.get(), "UnregisterProfile", g_variant_new("(o)", channel.object_path.c_str()),
           [](Variant, auto) {});
    }
    auto path = channel.device;
    entry.mapped().reset();
    ReleaseDevice(path);
  }

  void StartScan(Request request, Completion reply) {
    scan = request.scan;
    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
    g_variant_builder_add(&filter, "{sv}", "DuplicateData", g_variant_new_boolean(TRUE));
    if (!request.advertised_services.empty()) {
      GVariantBuilder uuids;
      g_variant_builder_init(&uuids, G_VARIANT_TYPE("as"));
      for (const auto& uuid : request.advertised_services)
        g_variant_builder_add(&uuids, "s", uuid.c_str());
      g_variant_builder_add(&filter, "{sv}", "UUIDs", g_variant_builder_end(&uuids));
    }
    Call(adapter.get(), "SetDiscoveryFilter", g_variant_new("(a{sv})", &filter),
         [weak = weak_from_this(), token = scan, target = adapter, reply = std::move(reply)]
         (Variant, std::optional<BluetoothError> error) mutable {
      auto state = weak.lock();
      if (!state || state->scan != token)
        return;
      if (error) {
        reply(Reply::Failure(std::move(*error)));
        return;
      }
      state->scan_proxy = target;
      Call(target.get(), "StartDiscovery", nullptr, [reply = std::move(reply)](Variant, auto error) {
        reply(error ? Reply::Failure(std::move(*error)) : Ok());
      });
    });
  }

  void StopScan(Token token) {
    std::erase_if(queued, [token](const auto& item) { return item.second.first.scan == token; });
    if (scan != token)
      return;
    scan = 0;
    if (auto target = std::exchange(scan_proxy, {}))
      Call(target.get(), "StopDiscovery", nullptr, [](Variant, auto) {});
  }

  Cancellation DeviceRequest(Request request, Completion reply) {
    auto device = Proxy(request.device, device_interface);
    if (!device || !OnAdapter(device.get())) {
      reply(Fail(Code::DeviceUnavailable, "BlueZ no longer recognizes this Bluetooth device"));
      return {};
    }
    if (request.operation == Operation::Restore) {
      reply(Ok(Describe(device.get())));
    } else if (request.operation == Operation::Bond) {
      reply(Ok(Flag(device.get(), "Paired") ? BondState::Bonded : BondState::NotBonded));
    } else if (request.operation == Operation::Pair) {
      if (Flag(device.get(), "Paired")) {
        reply(Ok());
        return {};
      }
      Call(device.get(), "Pair", nullptr, [reply = std::move(reply)](Variant, auto error) {
        if (error && !error->platform_domain.ends_with(".AlreadyExists"))
          reply(Reply::Failure(std::move(*error)));
        else
          reply(Ok());
      });
      return [device] { Call(device.get(), "CancelPairing", nullptr, [](Variant, auto) {}); };
    } else {
      const bool connected = Flag(device.get(), "Connected");
      const bool owned = held_connections.erase(request.device) != 0;
      auto& link = links.emplace(request.link, Link{.path = request.device, .device = device,
                                                    .connecting = std::move(reply),
                                                    .owns_connection = !connected || owned})
                       .first->second;
      if (connected) {
        link.waiting_for_connect_reply = false;
        TryConnected(link);
      } else {
        Call(device.get(), "Connect", nullptr, [weak = weak_from_this(), id = request.link](Variant, auto error) {
          auto state = weak.lock();
          if (!state)
            return;
          const auto found = state->links.find(id);
          if (found == state->links.end())
            return;
          auto& link = found->second;
          link.waiting_for_connect_reply = false;
          if (error && !error->platform_domain.ends_with(".AlreadyConnected")) {
            auto callback = std::exchange(link.connecting, {});
            if (callback)
              callback(Reply::Failure(std::move(*error)));
          } else {
            state->TryConnected(link);
          }
        });
      }
    }
    return {};
  }

  void TryConnected(Link& link) {
    if (link.connecting && !link.waiting_for_connect_reply && Flag(link.device.get(), "Connected") &&
        Flag(link.device.get(), "ServicesResolved")) {
      link.connected = true;
      auto reply = std::exchange(link.connecting, {});
      reply(Ok());
    }
  }

  Cancellation AttributeRequest(Request request, Completion reply) {
    const auto found = links.find(request.link);
    if (found == links.end() || !found->second.connected) {
      reply(Fail(Code::Disconnected, "The BlueZ GATT session is closed"));
      return {};
    }
    auto& link = found->second;
    if (request.operation == Operation::Discover) {
      auto cancel = OwnObject(g_cancellable_new());
      Discover(std::move(request), std::move(reply), cancel);
      return [cancel] { g_cancellable_cancel(cancel.get()); };
    }
    const auto attribute = link.attributes.find(request.attribute);
    if (attribute == link.attributes.end()) {
      reply(Fail(Code::StaleAttribute, "The BlueZ GATT attribute is stale"));
      return {};
    }
    auto proxy = attribute->second;
    auto cancel = OwnObject(g_cancellable_new());
    if (request.operation == Operation::Read) {
      if (link.notifications.contains(request.attribute)) {
        reply(Fail(Code::Busy, "Stop notifications before reading this characteristic on BlueZ"));
        return {};
      }
      Call(proxy.get(), "ReadValue", g_variant_new("(@a{sv})", EmptyOptions()),
           [reply = std::move(reply)](Variant value, auto error) {
        if (error)
          reply(Reply::Failure(std::move(*error)));
        else
          reply(Ok(ReadBytes(OwnVariant(g_variant_get_child_value(value.get(), 0)).get())));
      }, cancel.get());
    } else if (request.operation == Operation::Write) {
      auto bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, request.value.data(), request.value.size(), 1);
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
      if (!request.descriptor) {
        g_variant_builder_add(&options, "{sv}", "type",
                              g_variant_new_string(request.write_mode == GattWriteMode::WithResponse
                                                       ? "request" : "command"));
      }
      Call(proxy.get(), "WriteValue", g_variant_new("(@aya{sv})", bytes, &options),
           [reply = std::move(reply)](Variant, auto error) {
        reply(error ? Reply::Failure(std::move(*error)) : Ok());
      }, cancel.get());
    } else {
      Call(proxy.get(), request.enabled ? "StartNotify" : "StopNotify", nullptr,
           [weak = weak_from_this(), request, reply = std::move(reply)](Variant, auto error) {
        if (error) {
          reply(Reply::Failure(std::move(*error)));
        } else if (auto state = weak.lock()) {
          if (auto found = state->links.find(request.link); found != state->links.end()) {
            if (request.enabled)
              found->second.notifications.insert(request.attribute);
            else
              found->second.notifications.erase(request.attribute);
          }
          reply(Ok());
        }
      }, cancel.get());
    }
    return [cancel] { g_cancellable_cancel(cancel.get()); };
  }

  void Discover(Request request, Completion reply, std::shared_ptr<GCancellable> cancel) {
    const auto found = links.find(request.link);
    if (found == links.end())
      return;
    auto& link = found->second;
    if (!link.notifications.empty()) {
      const auto attribute = *link.notifications.begin();
      auto proxy = link.attributes.at(attribute);
      Call(proxy.get(), "StopNotify", nullptr,
           [weak = weak_from_this(), request, attribute, reply = std::move(reply), cancel](Variant, auto error) mutable {
        if (auto state = weak.lock()) {
          if (auto found = state->links.find(request.link); found != state->links.end()) {
            if (error && !error->platform_domain.ends_with(".UnknownObject")) {
              state->events(ServicesChanged{request.link});
              reply(Reply::Failure(std::move(*error)));
            } else {
              found->second.notifications.erase(attribute);
              state->Discover(std::move(request), std::move(reply), std::move(cancel));
            }
          }
        }
      }, cancel.get());
      return;
    }
    if (!Flag(link.device.get(), "ServicesResolved")) {
      reply(Fail(Code::Unavailable, "BlueZ is still resolving GATT services"));
      return;
    }
    link.attributes.clear();
    const auto characteristics = Objects(characteristic_interface);
    const auto descriptors = Objects(descriptor_interface);
    Discovery discovery;
    for (const auto& service : Objects(service_interface)) {
      if (String(service.get(), "Device") != link.path)
        continue;
      RemoteService output{String(service.get(), "UUID"), {}};
      const std::string_view service_path = g_dbus_proxy_get_object_path(service.get());
      for (const auto& characteristic : characteristics) {
        if (String(characteristic.get(), "Service") != service_path)
          continue;
        const std::string_view attribute_path = g_dbus_proxy_get_object_path(characteristic.get());
        const auto flags = Strings(Property(characteristic.get(), "Flags").get());
        const auto has = [&](std::string_view flag) {
          return std::find(flags.begin(), flags.end(), flag) != flags.end();
        };
        RemoteCharacteristic attr{next++, String(characteristic.get(), "UUID"),
                                  {has("read"), has("write"), has("write-without-response"),
                                   has("notify"), has("indicate")}, std::vector<RemoteDescriptor>{}};
        link.attributes.emplace(attr.id, characteristic);
        for (const auto& descriptor : descriptors) {
          if (String(descriptor.get(), "Characteristic") == attribute_path) {
            const auto id = next++;
            link.attributes.emplace(id, descriptor);
            attr.descriptors->push_back({id, String(descriptor.get(), "UUID")});
          }
        }
        output.characteristics.push_back(std::move(attr));
      }
      discovery.services.push_back(std::move(output));
    }
    reply(Ok(std::move(discovery)));
  }

  void Disconnect(Token link) {
    std::erase_if(queued, [link](const auto& item) { return item.second.first.link == link; });
    auto entry = links.extract(link);
    if (entry.empty())
      return;
    const auto& removed = entry.mapped();
    CleanupNotifications(removed);
    if (removed.owns_connection)
      held_connections.insert_or_assign(removed.path, removed.device);
    ReleaseDevice(removed.path);
  }

  void ReleaseDevice(const std::string& path) {
    if (std::ranges::any_of(channels, [&](const auto& entry) { return entry.second->device == path; }) ||
        std::ranges::any_of(links, [&](const auto& entry) { return entry.second.path == path; }))
      return;
    if (auto owner = held_connections.extract(path); !owner.empty()) {
      retiring_devices.insert(path);
      Call(owner.mapped().get(), "Disconnect", nullptr, [weak = weak_from_this(), path](Variant, auto) {
        if (auto state = weak.lock()) {
          state->retiring_devices.erase(path);
          auto pending = std::ranges::find_if(state->queued, [&](const auto& entry) {
            return entry.second.first.operation == Operation::Connect && entry.second.first.device == path;
          });
          if (pending != state->queued.end()) {
            auto request = state->queued.extract(pending);
            state->Invoke(std::move(request.mapped().first), std::move(request.mapped().second));
          }
        }
      });
    }
  }

  static void CleanupNotifications(const Link& link) {
    for (const auto attribute : link.notifications) {
      if (const auto found = link.attributes.find(attribute); found != link.attributes.end())
        Call(found->second.get(), "StopNotify", nullptr, [](Variant, auto) {});
    }
  }

  BackendEvents events;
  std::shared_ptr<GDBusObjectManager> manager;
  std::shared_ptr<GDBusProxy> adapter;
  std::shared_ptr<GDBusProxy> scan_proxy;
  AdapterStatus status = AdapterStatus::Unknown;
  bool initializing = true;
  Token next = 1;
  Token scan = 0;
  std::map<Token, std::pair<Request, Completion>> queued;
  std::map<Token, Link> links;
  std::map<Token, std::unique_ptr<SocketChannel>> channels;
  std::map<std::string, std::shared_ptr<GDBusProxy>> held_connections;
  std::set<std::string> retiring_devices;
};

class LinuxBackend final : public Backend {
public:
  explicit LinuxBackend(BackendEvents events) : state_(std::make_shared<State>(std::move(events))) {
    peripheral_ = CreateLinuxPeripheral(state_->events, [weak = std::weak_ptr<State>(state_)](const char* name) {
      if (auto state = weak.lock(); state && state->adapter)
        return state->Proxy(g_dbus_proxy_get_object_path(state->adapter.get()), name);
      return std::shared_ptr<GDBusProxy>{};
    });
    state_->Initialize();
  }

  AdapterStatus Status() const override {
    return state_->status;
  }

  BluetoothCapabilities PeripheralCapabilities() const override {
    return peripheral_->Capabilities();
  }

  void StopAdvertising(Token id) override {
    peripheral_->StopAdvertising(id);
  }

  void CloseServer(Token id) override {
    peripheral_->CloseServer(id);
  }

  Cancellation Invoke(Request request, Completion result) override {
    if (IsPeripheralOperation(request.operation))
      return peripheral_->Invoke(std::move(request), std::move(result));
    return state_->Invoke(std::move(request), std::move(result));
  }

  void StopScan(Token scan) override {
    state_->StopScan(scan);
  }

  void Disconnect(Token link) override {
    state_->Disconnect(link);
  }

  void CloseChannel(Token id) override {
    state_->CloseChannel(id);
  }

private:
  std::shared_ptr<State> state_;
  std::unique_ptr<PeripheralBackend> peripheral_;
};

} // namespace

BluetoothCapabilities PlatformCapabilities() {
  return {.can_scan = true, .can_restore_device = true, .can_pair = true, .can_query_bond = true,
          .can_use_gatt_client = true, .can_access_descriptors = true, .can_list_paired_rfcomm_devices = true,
          .can_connect_rfcomm = true, .can_connect_le_l2cap = true};
}

void RegisterPlatform(ApplicationContext&) {}

std::unique_ptr<Backend> CreateBackend(TaskScope, BackendEvents events) {
  return std::make_unique<LinuxBackend>(std::move(events));
}

} // namespace huxerui::bluetooth::detail
