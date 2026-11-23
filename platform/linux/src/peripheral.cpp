#include "backend.h"

#include <gio/gio.h>

#include <algorithm>
#include <utility>

namespace huxerui::bluetooth::detail {

namespace {

using Code = BluetoothErrorCode;
constexpr char advertisement_interface[] = "org.bluez.LEAdvertisement1";
constexpr char service_interface[] = "org.bluez.GattService1";
constexpr char characteristic_interface[] = "org.bluez.GattCharacteristic1";
constexpr char object_manager[] = "org.freedesktop.DBus.ObjectManager";
constexpr char advertising_manager[] = "org.bluez.LEAdvertisingManager1";
constexpr char gatt_manager[] = "org.bluez.GattManager1";

GDBusNodeInfo* Introspection() {
  static const auto info = std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)>(
      g_dbus_node_info_new_for_xml(R"xml(
<node>
  <interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  </interface>
  <interface name="org.bluez.LEAdvertisement1">
    <method name="Release"/>
    <property name="Type" type="s" access="read"/>
    <property name="ServiceUUIDs" type="as" access="read"/>
    <property name="LocalName" type="s" access="read"/>
  </interface>
  <interface name="org.bluez.GattService1">
    <property name="UUID" type="s" access="read"/>
    <property name="Primary" type="b" access="read"/>
  </interface>
  <interface name="org.bluez.GattCharacteristic1">
    <method name="ReadValue"><arg type="a{sv}" direction="in"/><arg type="ay" direction="out"/></method>
    <method name="WriteValue"><arg type="ay" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="StartNotify"/>
    <method name="StopNotify"/>
    <property name="UUID" type="s" access="read"/>
    <property name="Service" type="o" access="read"/>
    <property name="Flags" type="as" access="read"/>
    <property name="Value" type="ay" access="read"/>
    <property name="Notifying" type="b" access="read"/>
  </interface>
</node>
)xml", nullptr), g_dbus_node_info_unref);
  return info.get();
}

GVariant* ByteArray(const Bytes& bytes) {
  return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes.data(), bytes.size(), 1);
}

Bytes ReadBytes(GVariant* value) {
  gsize count = 0;
  auto* first = static_cast<const std::byte*>(g_variant_get_fixed_array(value, &count, 1));
  return count ? Bytes(first, first + count) : Bytes{};
}

BluetoothError BusError(GError* error, bool advertising) {
  const std::unique_ptr<char, decltype(&g_free)> name(g_dbus_error_get_remote_error(error), g_free);
  const std::string domain = name ? name.get() : g_quark_to_string(error->domain);
  auto code = Code::PlatformFailure;
  if (domain.ends_with(".NotAuthorized") || domain.ends_with(".AccessDenied"))
    code = Code::PermissionDenied;
  else if (domain.ends_with(".AlreadyExists") || domain.ends_with(".InProgress") ||
           (advertising && domain.ends_with(".NotPermitted")))
    code = Code::Busy;
  else if (domain.ends_with(".InvalidLength"))
    code = Code::PayloadTooLarge;
  else if (domain.ends_with(".NotSupported") || domain.ends_with(".UnknownMethod"))
    code = Code::Unsupported;
  else if (domain.ends_with(".NotReady") || domain.ends_with(".NameHasNoOwner") ||
           domain.ends_with(".ServiceUnknown"))
    code = Code::Unavailable;
  else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    code = Code::TimedOut;
  return Error(code, error->message, domain, error->code);
}

struct ExportedObject {
  const char* interface;
  guint registration = 0;
  std::string uuid;
  std::string service;
  Token attribute = 0;
  GattCharacteristicProperties properties;
  Bytes value;
  bool subscribed = false;
};

struct PendingAttReply {
  GDBusMethodInvocation* invocation;

  ~PendingAttReply() {
    if (invocation) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "Local GATT request stopped");
    }
  }
};

class Registration final : public std::enable_shared_from_this<Registration> {
public:
  Registration(std::shared_ptr<GDBusProxy> proxy, Request request, Completion completion, BackendEvents events)
      : proxy_(std::move(proxy)), bus_(g_dbus_proxy_get_connection(proxy_.get())), request_(std::move(request)),
        completion_(std::move(completion)), events_(std::move(events)),
        root_("/org/huxerui/Bluetooth/session" + std::to_string(request_.peripheral)),
        advertising_(request_.operation == Operation::Advertise) {}

  ~Registration() {
    Unexport();
  }

  Token Id() const { return request_.peripheral; }

  bool Retired() const { return retired_; }

  void Start() {
    const std::unique_ptr<char, decltype(&g_free)> owner(g_dbus_proxy_get_name_owner(proxy_.get()), g_free);
    if (!owner) {
      Failed(Error(Code::Unavailable, "The BlueZ service is unavailable"));
      return;
    }
    owner_ = owner.get();
    objects_.emplace(root_, ExportedObject{advertising_ ? advertisement_interface : object_manager});
    if (!advertising_) {
      std::size_t index = 0;
      for (const auto& service : request_.local_services) {
        const auto service_path = root_ + "/service" + std::to_string(++index);
        objects_.emplace(service_path, ExportedObject{service_interface, 0, service.uuid});
        for (const auto& characteristic : service.characteristics) {
          objects_.emplace(service_path + "/char" + std::to_string(characteristic.id),
                           ExportedObject{characteristic_interface, 0, characteristic.uuid, service_path,
                                          characteristic.id, characteristic.properties});
        }
      }
    }
    for (auto& [path, object] : objects_) {
      static const GDBusInterfaceVTable table{MethodCall, GetProperty, nullptr, {nullptr}};
      GError* error = nullptr;
      object.registration = g_dbus_connection_register_object(
          bus_, path.c_str(), g_dbus_node_info_lookup_interface(Introspection(), object.interface),
          &table, this, nullptr, &error);
      if (!object.registration) {
        const std::unique_ptr<GError, decltype(&g_error_free)> failure(error, g_error_free);
        Failed(BusError(failure.get(), advertising_));
        return;
      }
    }
    pending_ = true;
    g_dbus_proxy_call(proxy_.get(), advertising_ ? "RegisterAdvertisement" : "RegisterApplication",
                      g_variant_new("(o@a{sv})", root_.c_str(),
                                    g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
                      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, Registered,
                      new std::shared_ptr<Registration>(shared_from_this()));
  }

  void Close() {
    if (std::exchange(closing_, true) || retired_)
      return;
    completion_ = {};
    if (!pending_)
      Unregister();
  }

  void Notify(Token attribute, Bytes value, Completion completion) {
    if (closing_ || pending_ || retired_) {
      completion(Fail(Code::Unavailable, "The BlueZ local GATT server is unavailable"));
      return;
    }
    const auto found = std::ranges::find_if(objects_, [attribute](const auto& item) {
      return item.second.attribute == attribute;
    });
    if (found == objects_.end()) {
      completion(Fail(Code::InvalidArgument, "Unknown local GATT characteristic"));
      return;
    }
    if (!found->second.subscribed) {
      completion(Ok());
      return;
    }
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "Value", ByteArray(value));
    GError* error = nullptr;
    if (g_dbus_connection_emit_signal(bus_, owner_.c_str(), found->first.c_str(), "org.freedesktop.DBus.Properties",
                                     "PropertiesChanged",
                                     g_variant_new("(s@a{sv}@as)", characteristic_interface,
                                                   g_variant_builder_end(&properties),
                                                   g_variant_new_strv(nullptr, 0)), &error)) {
      found->second.value = std::move(value);
      completion(Ok());
    } else {
      const std::unique_ptr<GError, decltype(&g_error_free)> failure(error, g_error_free);
      completion(Reply::Failure(BusError(failure.get(), advertising_)));
    }
  }

private:
  void Unexport() {
    for (auto& [path, object] : objects_)
      if (object.registration)
        g_dbus_connection_unregister_object(bus_, object.registration);
    objects_.clear();
    retired_ = true;
  }

  void Failed(BluetoothError error) {
    Unexport();
    if (auto completion = std::exchange(completion_, {}))
      completion(Reply::Failure(std::move(error)));
  }

  static void Registered(GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<std::shared_ptr<Registration>> retained(static_cast<std::shared_ptr<Registration>*>(data));
    auto& current = **retained;
    GError* error = nullptr;
    auto* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    current.pending_ = false;
    if (!reply) {
      const std::unique_ptr<GError, decltype(&g_error_free)> failure(error, g_error_free);
      current.Failed(BusError(failure.get(), current.advertising_));
      return;
    }
    g_variant_unref(reply);
    if (current.closing_)
      current.Unregister();
    else if (auto completion = std::exchange(current.completion_, {}))
      completion(Ok());
  }

  void Unregister() {
    g_dbus_proxy_call(proxy_.get(), advertising_ ? "UnregisterAdvertisement" : "UnregisterApplication",
                      g_variant_new("(o)", root_.c_str()), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                      [](GObject* source, GAsyncResult* result, gpointer data) {
      const std::unique_ptr<std::shared_ptr<Registration>> retained(static_cast<std::shared_ptr<Registration>*>(data));
      if (auto* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, nullptr))
        g_variant_unref(reply);
      (*retained)->Unexport();
    }, new std::shared_ptr<Registration>(shared_from_this()));
  }

  GVariant* Property(const ExportedObject& object, std::string_view name) const {
    if (name == "UUID")
      return g_variant_new_string(object.uuid.c_str());
    if (name == "Primary")
      return g_variant_new_boolean(true);
    if (name == "Service")
      return g_variant_new_object_path(object.service.c_str());
    if (name == "Notifying")
      return g_variant_new_boolean(object.subscribed);
    if (name == "Value")
      return ByteArray(object.value);
    if (name == "Type")
      return g_variant_new_string(request_.advertisement.connectable.value_or(request_.server != 0)
                                     ? "peripheral" : "broadcast");
    if (name == "LocalName" && request_.advertisement.local_name)
      return g_variant_new_string(request_.advertisement.local_name->c_str());
    if (name == "ServiceUUIDs" || name == "Flags") {
      GVariantBuilder list;
      g_variant_builder_init(&list, G_VARIANT_TYPE("as"));
      if (name == "ServiceUUIDs") {
        for (const auto& uuid : request_.advertisement.service_uuids)
          g_variant_builder_add(&list, "s", uuid.c_str());
      } else {
        if (object.properties.can_read)
          g_variant_builder_add(&list, "s", "read");
        if (object.properties.can_write_with_response)
          g_variant_builder_add(&list, "s", "write");
        if (object.properties.can_notify)
          g_variant_builder_add(&list, "s", "notify");
      }
      return g_variant_builder_end(&list);
    }
    return nullptr;
  }

  static GVariant* GetProperty(GDBusConnection*, const gchar* sender, const gchar* path, const gchar*,
                               const gchar* name, GError** error, gpointer data) {
    const auto& self = *static_cast<Registration*>(data);
    if (sender && self.owner_ == sender) {
      if (const auto found = self.objects_.find(path); found != self.objects_.end()) {
        if (auto* value = self.Property(found->second, name))
          return value;
      }
    }
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Property is unavailable");
    return nullptr;
  }

  GVariant* ManagedObjects() const {
    GVariantBuilder tree;
    g_variant_builder_init(&tree, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    for (const auto& [path, object] : objects_) {
      if (path == root_)
        continue;
      GVariantBuilder properties;
      g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
      const auto* info = g_dbus_node_info_lookup_interface(Introspection(), object.interface);
      for (auto** item = info->properties; *item; ++item) {
        if (auto* value = Property(object, (*item)->name))
          g_variant_builder_add(&properties, "{sv}", (*item)->name, value);
      }
      GVariantBuilder interfaces;
      g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      g_variant_builder_add(&interfaces, "{s@a{sv}}", object.interface, g_variant_builder_end(&properties));
      g_variant_builder_add(&tree, "{o@a{sa{sv}}}", path.c_str(), g_variant_builder_end(&interfaces));
    }
    return g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&tree));
  }

  static void AttReply(GDBusMethodInvocation* invocation, AttStatus status, bool write, const Bytes& bytes) {
    if (status == AttStatus::Success) {
      g_dbus_method_invocation_return_value(invocation, write ? nullptr : g_variant_new("(@ay)", ByteArray(bytes)));
      return;
    }
    const char* name = "org.bluez.Error.Failed";
    switch (status) {
      case AttStatus::ReadNotPermitted:
      case AttStatus::WriteNotPermitted: name = "org.bluez.Error.NotPermitted"; break;
      case AttStatus::RequestNotSupported: name = "org.bluez.Error.NotSupported"; break;
      case AttStatus::InvalidOffset: name = "org.bluez.Error.InvalidOffset"; break;
      case AttStatus::InvalidValueLength: name = "org.bluez.Error.InvalidValueLength"; break;
      default: break;
    }
    g_dbus_method_invocation_return_dbus_error(invocation, name, "Local GATT request rejected");
  }

  void AttributeMethod(ExportedObject& object, std::string_view method, GVariant* arguments,
                       GDBusMethodInvocation* invocation) {
    if (closing_) {
      AttReply(invocation, AttStatus::UnlikelyError, true, {});
      return;
    }
    if (method == "StartNotify" || method == "StopNotify") {
      const bool enabled = method == "StartNotify";
      if (!object.properties.can_notify) {
        AttReply(invocation, AttStatus::RequestNotSupported, true, {});
        return;
      }
      if (object.subscribed != enabled) {
        object.subscribed = enabled;
        events_(ServerSubscription{Id(), object.attribute, enabled});
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    const bool write = method == "WriteValue";
    if (!write && method != "ReadValue") {
      AttReply(invocation, AttStatus::RequestNotSupported, true, {});
      return;
    }
    using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
    const Variant options(g_variant_get_child_value(arguments, write ? 1 : 0), g_variant_unref);
    guint16 offset = 0;
    guint16 mtu = 0;
    g_variant_lookup(options.get(), "offset", "q", &offset);
    g_variant_lookup(options.get(), "mtu", "q", &mtu);
    Bytes value;
    if (write) {
      const gchar* type = nullptr;
      gboolean prepare = false;
      g_variant_lookup(options.get(), "type", "&s", &type);
      g_variant_lookup(options.get(), "prepare-authorize", "b", &prepare);
      if (prepare || (type && std::string_view(type) != "request")) {
        AttReply(invocation, AttStatus::RequestNotSupported, true, {});
        return;
      }
      const Variant payload(g_variant_get_child_value(arguments, 0), g_variant_unref);
      value = ReadBytes(payload.get());
    }
    auto keep = std::make_shared<PendingAttReply>(invocation);
    events_(ServerRequest{Id(), object.attribute, write, offset, std::move(value),
                          mtu > 0 ? std::optional<std::size_t>(mtu - 1) : std::nullopt,
                          [keep = std::move(keep), write](AttStatus status, Bytes response) {
      if (auto* invocation = std::exchange(keep->invocation, nullptr))
        AttReply(invocation, status, write, response);
    }});
  }

  static void MethodCall(GDBusConnection*, const gchar* sender, const gchar* path, const gchar*,
                         const gchar* method, GVariant* arguments, GDBusMethodInvocation* invocation, gpointer data) {
    auto& self = *static_cast<Registration*>(data);
    if (!sender || self.owner_ != sender) {
      g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                   "Only BlueZ can access local Bluetooth objects");
      return;
    }
    const auto found = self.objects_.find(path);
    if (found == self.objects_.end()) {
      AttReply(invocation, AttStatus::RequestNotSupported, true, {});
      return;
    }
    const std::string_view action(method);
    if (path == self.root_ && action == "GetManagedObjects") {
      g_dbus_method_invocation_return_value(invocation, self.ManagedObjects());
    } else if (path == self.root_ && action == "Release") {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      if (!self.closing_)
        self.events_(PeripheralFailure{self.Id(), Error(Code::Unavailable, "BlueZ released the advertisement")});
    } else {
      self.AttributeMethod(found->second, action, arguments, invocation);
    }
  }

  std::shared_ptr<GDBusProxy> proxy_;
  GDBusConnection* bus_;
  Request request_;
  Completion completion_;
  BackendEvents events_;
  std::string root_;
  std::string owner_;
  bool advertising_;
  bool pending_ = false;
  bool closing_ = false;
  bool retired_ = false;
  std::map<std::string, ExportedObject> objects_;
};

class LinuxPeripheralBackend final : public PeripheralBackend {
public:
  LinuxPeripheralBackend(BackendEvents events, std::function<std::shared_ptr<GDBusProxy>(const char*)> manager)
      : events_(std::move(events)), manager_(std::move(manager)) {}

  ~LinuxPeripheralBackend() override {
    if (advertisement_)
      advertisement_->Close();
    if (server_)
      server_->Close();
  }

  BluetoothCapabilities Capabilities() const override {
    const bool advertising = bool(manager_(advertising_manager));
    return {.can_advertise = advertising, .can_use_gatt_server = bool(manager_(gatt_manager)),
            .can_advertise_standalone = advertising};
  }

  Cancellation Invoke(Request request, Completion completion) override {
    if (request.operation == Operation::SendNotification) {
      if (server_ && server_->Id() == request.peripheral)
        server_->Notify(request.attribute, std::move(request.value), std::move(completion));
      else
        completion(Fail(Code::Disconnected, "The local GATT server is closed"));
      return {};
    }
    const bool advertising = request.operation == Operation::Advertise;
    if (const auto& name = request.advertisement.local_name; name &&
        !g_utf8_validate(name->data(), static_cast<gssize>(name->size()), nullptr)) {
      completion(Fail(Code::InvalidArgument, "The local name must be valid UTF-8"));
      return {};
    }
    auto& slot = advertising ? advertisement_ : server_;
    if (slot && !slot->Retired()) {
      completion(Fail(Code::Busy, "A BlueZ registration of this kind is already active"));
      return {};
    }
    auto manager = manager_(advertising ? advertising_manager : gatt_manager);
    if (!manager) {
      completion(Fail(Code::Unsupported, "The BlueZ adapter does not expose this peripheral capability"));
      return {};
    }
    slot = std::make_shared<Registration>(std::move(manager), std::move(request), std::move(completion), events_);
    slot->Start();
    return [weak = std::weak_ptr<Registration>(slot)] {
      if (auto current = weak.lock())
        current->Close();
    };
  }

  void StopAdvertising(Token id) override {
    if (advertisement_ && advertisement_->Id() == id)
      advertisement_->Close();
  }

  void CloseServer(Token id) override {
    if (server_ && server_->Id() == id)
      server_->Close();
  }

private:
  BackendEvents events_;
  std::function<std::shared_ptr<GDBusProxy>(const char*)> manager_;
  std::shared_ptr<Registration> advertisement_;
  std::shared_ptr<Registration> server_;
};

} // namespace

std::unique_ptr<PeripheralBackend> CreateLinuxPeripheral(
    BackendEvents events, std::function<std::shared_ptr<GDBusProxy>(const char*)> manager) {
  return std::make_unique<LinuxPeripheralBackend>(std::move(events), std::move(manager));
}

} // namespace huxerui::bluetooth::detail
