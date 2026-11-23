#include <huxerui/bluetooth.h>
#include <huxerui/huxerui.h>

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#endif

#include <algorithm>
#include <charconv>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace huxerui;
using namespace huxerui::bluetooth;

namespace {

struct DeviceRow {
  Device device;
  std::string label;

  bool operator==(const DeviceRow&) const = default;
};

struct CharacteristicPath {
  std::string service;
  std::string characteristic;
};

constexpr char local_service_uuid[] = "e1120001-14f2-43f3-a98b-cfd9b47fd9bb";
constexpr char local_value_uuid[] = "e1120002-14f2-43f3-a98b-cfd9b47fd9bb";

struct LocalPeripheral {
  State<bool> server_open{false};
  State<bool> opening{false};
  State<bool> advertising{false};
  State<bool> starting_advertisement{false};
  State<bool> subscribed{false};
  State<int> value{0};
  std::optional<GattServerSession> server;
  std::optional<AdvertisingSession> advertisement;
};

struct Preview {
  State<AdapterStatus> adapter{AdapterStatus::Unknown};
  State<std::vector<DeviceRow>> devices{std::vector<DeviceRow>{}};
  State<std::optional<Device>> selected{std::optional<Device>{}};
  State<std::string> saved_id{""};
  State<std::shared_ptr<const std::vector<GattService>>> services{std::make_shared<const std::vector<GattService>>()};
  State<std::optional<GattCharacteristic>> characteristic{std::optional<GattCharacteristic>{}};
  State<TextEditingValue> filter{TextEditingValue{}};
  State<TextEditingValue> payload{TextEditingValue{}};
  State<std::string> status{"Choose Scan or Select device"};
  State<std::string> value{""};
  State<bool> scanning{false};
  State<bool> connecting{false};
  State<bool> connected{false};
  State<bool> subscribed{false};
  State<bool> auto_reconnect{false};

  std::optional<ScanSession> scan;
  std::optional<GattSession> session;
  std::optional<GattSubscription> subscription;
  std::optional<CharacteristicPath> characteristic_path;
  std::optional<File> selection_file;
  std::optional<std::string> pending_selection;
  std::uint64_t selection_revision = 0;
  std::uint64_t connection_generation = 0;
  bool saving_selection = false;
  bool recovering = false;
  TaskHandle operation;
  LocalPeripheral peripheral;

  void StopScan() {
    scan.reset();
    scanning = false;
  }

  void ClearAttributes() {
    subscription.reset();
    subscribed = false;
    characteristic_path.reset();
    characteristic = std::optional<GattCharacteristic>{};
    services = std::make_shared<const std::vector<GattService>>();
    value = std::string{};
  }

  void Disconnect() {
    ++connection_generation;
    operation.Cancel();
    ClearAttributes();
    if (session)
      session->Disconnect();
    session.reset();
    connecting = false;
    connected = false;
    recovering = false;
  }

  void Report(const BluetoothError& error) {
    status = error.platform_code ? error.message + " (" + std::to_string(*error.platform_code) + ")" : error.message;
  }

  void Receive(ScanEvent event) {
    if (const auto* observation = std::get_if<ScanObservation>(&event)) {
      auto name = observation->advertisement.local_name.value_or(observation->device.Name().value_or("Unnamed device"));
      if (observation->advertisement.rssi_dbm)
        name += " · " + std::to_string(*observation->advertisement.rssi_dbm) + " dBm";
      std::vector<DeviceRow> rows = devices;
      const auto found = std::ranges::find(rows, observation->device, &DeviceRow::device);
      DeviceRow row{observation->device, std::move(name)};
      if (found == rows.end())
        rows.push_back(std::move(row));
      else
        *found = std::move(row);
      devices = std::move(rows);
      return;
    }
    StopScan();
    if (const auto* error = std::get_if<BluetoothError>(&event))
      Report(*error);
    else
      status = "Scan finished";
  }
};

const char* StatusText(AdapterStatus status) {
  switch (status) {
    case AdapterStatus::Unavailable: return "Bluetooth unavailable";
    case AdapterStatus::PoweredOff: return "Bluetooth off";
    case AdapterStatus::PoweredOn: return "Bluetooth on";
    case AdapterStatus::Unknown: return "Adapter status unknown";
  }
  return "Adapter status unknown";
}

std::optional<Bytes> ParseHex(std::string_view input) {
  Bytes bytes;
  std::string digits;
  for (const char letter : input) {
    if (letter != ' ' && letter != '\t' && letter != '\n' && letter != '\r')
      digits.push_back(letter);
  }
  if (digits.size() % 2 != 0)
    return std::nullopt;
  for (std::size_t index = 0; index < digits.size(); index += 2) {
    unsigned value = 0;
    const auto* first = digits.data() + index;
    const auto result = std::from_chars(first, first + 2, value, 16);
    if (result.ec != std::errc{} || result.ptr != first + 2)
      return std::nullopt;
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

std::string FormatHex(const Bytes& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    if (!result.empty())
      result += ' ';
    result += digits[value >> 4u];
    result += digits[value & 15u];
  }
  return result.empty() ? "Empty value" : result;
}

Task<bool> Authorize(ApplicationHandle app, Permission permission, bool allow_prompt = true) {
#if defined(__ANDROID__) || defined(__APPLE__)
  auto status = co_await app.CheckPermissionAsync(permission);
  if (allow_prompt && (status == PermissionStatus::NotDetermined || status == PermissionStatus::Denied))
    status = co_await app.RequestPermissionAsync(permission);
  co_return status == PermissionStatus::Granted || status == PermissionStatus::Limited ||
            status == PermissionStatus::SystemDisabled;
#else
  (void)app;
  (void)permission;
  (void)allow_prompt;
  co_return true;
#endif
}

#if defined(__ANDROID__)
constexpr char advertising_permission_module[] = "bluetooth/PreviewAdvertisingPermission";

class AndroidAdvertisingPermission final {
public:
  explicit AndroidAdvertisingPermission(PlatformChannel channel) : channel_(std::move(channel)) {}

  ~AndroidAdvertisingPermission() {
    channel_.Close();
  }

  Task<bool> Request() const {
    return AwaitCallback<bool>([channel = channel_](CallbackCompletion<bool> completion) {
      const auto ticket = channel.Invoke("request", {}, [completion](PlatformResult<PlatformPayload> result) {
        const auto* value = std::get_if<PlatformPayload>(&result);
        completion.Complete(value && value->AsBoolean());
      });
      return [channel, ticket] { (void)channel.Cancel(ticket); };
    });
  }

private:
  PlatformChannel channel_;
};
#endif

Task<bool> AuthorizeAdvertising(ApplicationHandle app) {
#if defined(__ANDROID__)
  (void)app;
  const auto permission = OpenPlatformModule<std::shared_ptr<AndroidAdvertisingPermission>>(
      advertising_permission_module);
  co_return co_await permission->Request();
#else
  co_return co_await Authorize(app, Permission::BluetoothConnect);
#endif
}

void SaveSelection(const std::shared_ptr<Preview>& model, TaskScope tasks, std::optional<Device> device) {
  ++model->selection_revision;
  auto id = device ? device->Id() : std::string{};
  model->selected = std::move(device);
  model->saved_id = id;
  if (!model->selection_file)
    return;
  model->pending_selection = std::move(id);
  if (model->saving_selection)
    return;
  model->saving_selection = true;
  tasks.Launch([model, file = *model->selection_file]() -> Task<void> {
    const File staged(file.Path() + ".pending");
    while (model->pending_selection) {
      auto pending = std::exchange(model->pending_selection, std::nullopt);
      const bool written = co_await staged.WriteStringAsync(std::move(*pending));
      const bool saved = written && (co_await staged.MoveToAsync(file, true));
      if (!saved && !model->pending_selection)
        model->status = "Could not save device selection";
    }
    model->saving_selection = false;
  });
}

Task<void> RestoreSelection(std::shared_ptr<Preview> model, std::shared_ptr<Bluetooth> bluetooth,
                            ApplicationHandle app, bool load_from_storage) {
  const auto revision = model->selection_revision;
  if (load_from_storage) {
    if (!model->selection_file || revision != 0)
      co_return;
    auto stored = co_await model->selection_file->ReadStringAsync();
    if (model->selection_revision != revision || !stored.Succeeded())
      co_return;
    model->saved_id = std::move(stored).Value();
  }
  const std::string id = model->saved_id;
  if (id.empty() || !bluetooth->Capabilities().can_restore_device)
    co_return;
  if (!co_await Authorize(app, Permission::BluetoothConnect, !load_from_storage)) {
    if (!load_from_storage && model->selection_revision == revision)
      model->status = "Bluetooth connect permission was denied";
    co_return;
  }
  if (model->selection_revision != revision)
    co_return;
  auto restored = co_await bluetooth->RestoreDeviceAsync(id);
  if (model->selection_revision != revision)
    co_return;
  if (restored.Succeeded()) {
    model->selected = std::move(restored).Value();
    model->status = "Saved device restored";
  } else {
    model->Report(restored.Error());
  }
}

BluetoothResult<GattCharacteristic> FindCharacteristic(std::span<const GattService> services,
                                                       const CharacteristicPath& path) {
  using Result = BluetoothResult<GattCharacteristic>;
  const GattCharacteristic* match = nullptr;
  for (const auto& service : services) {
    if (service.uuid != path.service)
      continue;
    for (const auto& characteristic : service.characteristics) {
      if (characteristic.Uuid() != path.characteristic)
        continue;
      if (match)
        return Result::Failure({.code = BluetoothErrorCode::InvalidArgument,
                                .message = "The previous subscription matches more than one characteristic"});
      match = &characteristic;
    }
  }
  if (!match)
    return Result::Failure({.code = BluetoothErrorCode::UnsupportedProperty,
                            .message = "The previous subscription characteristic is unavailable"});
  return Result::Success(*match);
}

bool CanReconnect(BluetoothErrorCode code) {
  return code == BluetoothErrorCode::Disconnected || code == BluetoothErrorCode::DeviceUnavailable ||
         code == BluetoothErrorCode::TimedOut || code == BluetoothErrorCode::StaleAttribute;
}

void StartConnection(const std::shared_ptr<Preview>& model, const std::shared_ptr<Bluetooth>& bluetooth,
                     ApplicationHandle app, TaskScope tasks, Device device, bool automatic = false,
                     std::optional<CharacteristicPath> restore_path = {}) {
  if (model->connecting || model->connected)
    return;
  model->StopScan();
  const auto generation = ++model->connection_generation;
  model->connecting = true;
  model->recovering = automatic;
  model->status = automatic ? "Reconnecting" : "Connecting";
  auto on_event = [weak = std::weak_ptr<Preview>(model), service = std::weak_ptr<Bluetooth>(bluetooth),
                   app, tasks, device, generation](GattEvent event) {
    auto current = weak.lock();
    if (!current || current->connection_generation != generation)
      return;
    const auto* closed = std::get_if<GattDisconnected>(&event);
    if (!closed) {
      current->ClearAttributes();
      current->status = "Services changed. Discover again.";
      return;
    }
    const auto path = current->subscribed ? current->characteristic_path : std::nullopt;
    ++current->connection_generation;
    current->ClearAttributes();
    current->session.reset();
    current->connected = false;
    if (closed->error) {
      current->Report(*closed->error);
      if (auto bluetooth = service.lock(); bluetooth && current->auto_reconnect &&
          app.LifecycleState() == ApplicationLifecycleState::Active && CanReconnect(closed->error->code)) {
        StartConnection(current, bluetooth, app, tasks, device, true, path);
      }
    } else {
      current->status = "Disconnected";
    }
  };

  model->operation = tasks.Launch(
      [model, bluetooth, app, device = std::move(device), automatic, generation, restore_path = std::move(restore_path),
       on_event = std::move(on_event)]() mutable -> Task<void> {
        if (!co_await Authorize(app, Permission::BluetoothConnect, !automatic)) {
          if (model->connection_generation == generation) {
            model->connecting = false;
            model->recovering = false;
            model->status = "Bluetooth connect permission was denied";
          }
          co_return;
        }
        if (model->connection_generation != generation)
          co_return;
        if (!automatic) {
          auto connected = co_await bluetooth->ConnectAsync(std::move(device), {}, std::move(on_event));
          if (model->connection_generation != generation)
            co_return;
          model->connecting = false;
          if (connected.Succeeded()) {
            model->session.emplace(std::move(connected).Value());
            model->connected = true;
            model->status = "Connected";
          } else {
            model->Report(connected.Error());
          }
          co_return;
        }

        RestoreSubscriptions restore;
        if (restore_path) {
          restore = [path = *restore_path, weak = std::weak_ptr<Preview>(model), generation]
                    (GattSession& session, std::span<const GattService> services)
                    -> Task<BluetoothResult<std::vector<GattSubscription>>> {
            using Result = BluetoothResult<std::vector<GattSubscription>>;
            auto characteristic = FindCharacteristic(services, path);
            if (!characteristic.Succeeded())
              co_return Result::Failure(std::move(characteristic).Error());
            auto subscribed = co_await session.SubscribeAsync(std::move(characteristic).Value(),
                [weak, generation](Bytes value) {
                  if (auto current = weak.lock(); current && current->connection_generation == generation &&
                      current->connected && current->subscribed) {
                    current->value = FormatHex(value);
                  }
                });
            if (!subscribed.Succeeded())
              co_return Result::Failure(std::move(subscribed).Error());
            std::vector<GattSubscription> subscriptions;
            subscriptions.push_back(std::move(subscribed).Value());
            co_return Result::Success(std::move(subscriptions));
          };
        }
        auto recovered = co_await bluetooth->ConnectWithRetryAsync(
            std::move(device), {}, std::move(restore), std::move(on_event));
        if (model->connection_generation != generation)
          co_return;
        model->connecting = false;
        model->recovering = false;
        if (!recovered.Succeeded()) {
          model->Report(recovered.Error());
          co_return;
        }
        auto connection = std::move(recovered).Value();
        if (restore_path && !connection.subscriptions.empty()) {
          auto characteristic = FindCharacteristic(connection.services, *restore_path);
          if (characteristic.Succeeded()) {
            model->characteristic = std::move(characteristic).Value();
            model->characteristic_path = *restore_path;
            model->subscription.emplace(std::move(connection.subscriptions.front()));
            model->subscribed = true;
          }
        }
        model->session.emplace(std::move(connection.session));
        model->services = std::make_shared<const std::vector<GattService>>(std::move(connection.services));
        model->connected = true;
        model->status = "Connection restored";
      });
}

View AttributeControls(const std::shared_ptr<Preview>& model, TaskScope tasks) {
  const std::optional<GattCharacteristic> characteristic = model->characteristic;
  if (!characteristic || !model->session)
    return {};
  const auto properties = characteristic->Properties();
  const auto generation = model->connection_generation;
  const bool subscribed = model->subscribed;
  const auto write = [model, tasks, characteristic, generation](GattWriteMode mode) {
    auto bytes = ParseHex(model->payload->text);
    if (!bytes) {
      model->status = "Enter hexadecimal byte pairs";
      return;
    }
    tasks.Launch([model, characteristic, generation, mode, bytes = std::move(*bytes)]() mutable -> Task<void> {
      if (!model->session || model->connection_generation != generation)
        co_return;
      auto result = co_await model->session->WriteAsync(*characteristic, std::move(bytes), mode);
      if (model->connection_generation != generation)
        co_return;
      if (result.Succeeded())
        model->status = mode == GattWriteMode::WithResponse ? "Write acknowledged" : "Write accepted";
      else
        model->Report(result.Error());
    });
  };
  return Column {
    Text(characteristic->Uuid()),
    TextField(model->payload)
        .Label("Value in hex")
        .Placeholder("00 ff 2a")
        .OnChanged([model](const TextEditingValue& value) { model->payload = value; }),
    Flow {
      Button("Read").With(Enabled(properties.can_read)).OnClick([model, tasks, characteristic, generation] {
        tasks.Launch([model, characteristic, generation]() -> Task<void> {
          if (!model->session || model->connection_generation != generation)
            co_return;
          auto result = co_await model->session->ReadAsync(*characteristic);
          const std::optional<GattCharacteristic> selected = model->characteristic;
          if (model->connection_generation != generation || selected != characteristic)
            co_return;
          if (result.Succeeded())
            model->value = FormatHex(result.Value());
          else
            model->Report(result.Error());
        });
      }),
      Button("Write").With(Enabled(properties.can_write_with_response))
          .OnClick([write] { write(GattWriteMode::WithResponse); }),
      Button("Write without response").With(Enabled(properties.can_write_without_response))
          .OnClick([write] { write(GattWriteMode::WithoutResponse); }),
      Button(subscribed ? "Unsubscribe" : "Subscribe")
          .With(Enabled(properties.can_notify || properties.can_indicate))
          .OnClick([model, tasks, characteristic, subscribed, generation] {
            if (model->connection_generation != generation)
              return;
            if (subscribed) {
              model->subscription.reset();
              model->subscribed = false;
              return;
            }
            tasks.Launch([model, characteristic, generation]() -> Task<void> {
              if (!model->session || model->connection_generation != generation)
                co_return;
              auto result = co_await model->session->SubscribeAsync(*characteristic,
                  [weak = std::weak_ptr<Preview>(model), characteristic, generation](Bytes value) {
                    if (auto current = weak.lock(); current && current->connection_generation == generation) {
                      const std::optional<GattCharacteristic> selected = current->characteristic;
                      if (selected == characteristic)
                        current->value = FormatHex(value);
                    }
                  });
              const std::optional<GattCharacteristic> selected = model->characteristic;
              if (model->connection_generation != generation || selected != characteristic)
                co_return;
              if (!result.Succeeded()) {
                model->Report(result.Error());
              } else {
                model->subscription.emplace(std::move(result).Value());
                model->subscribed = true;
              }
            });
          }),
    }.With(Spacing(8.0F)),
    Text(model->value),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

Task<void> OpenLocalServer(std::shared_ptr<Preview> model, std::shared_ptr<Bluetooth> bluetooth, ApplicationHandle app) {
  auto& local = model->peripheral;
  if (!co_await Authorize(app, Permission::BluetoothConnect)) {
    local.opening = false;
    model->status = "Bluetooth connect permission was denied";
    co_return;
  }
  const auto weak = std::weak_ptr<Preview>(model);
  using ReadResult = Result<Bytes, GattRequestError>;
  using WriteResult = Result<void, GattRequestError>;
  LocalGattCharacteristic characteristic{
      .uuid = local_value_uuid,
      .properties = {.can_read = true, .can_write_with_response = true, .can_notify = true},
      .on_read = [weak] {
        if (auto current = weak.lock()) {
          const int value = current->peripheral.value;
          return ReadResult::Success({static_cast<std::byte>(value)});
        }
        return ReadResult::Failure(GattRequestError::UnlikelyError);
      },
      .on_write = [weak](Bytes value) {
        if (value.size() != 1)
          return WriteResult::Failure(GattRequestError::InvalidValueLength);
        if (auto current = weak.lock()) {
          current->peripheral.value = std::to_integer<int>(value.front());
          return WriteResult::Success();
        }
        return WriteResult::Failure(GattRequestError::UnlikelyError);
      },
      .on_subscription_changed = [weak](bool subscribed) {
        if (auto current = weak.lock())
          current->peripheral.subscribed = subscribed;
      },
  };
  std::vector<LocalGattService> services{{local_service_uuid, {std::move(characteristic)}}};
  auto opened = co_await bluetooth->OpenGattServerAsync(std::move(services), {}, [weak](PeripheralEndEvent event) {
    if (auto current = weak.lock()) {
      auto& local = current->peripheral;
      local.server.reset();
      local.server_open = false;
      local.subscribed = false;
      if (const auto* error = std::get_if<BluetoothError>(&event))
        current->Report(*error);
    }
  });
  local.opening = false;
  if (opened.Succeeded()) {
    local.server.emplace(std::move(opened).Value());
    local.server_open = true;
    model->status = "Local GATT server open";
  } else {
    model->Report(opened.Error());
  }
}

Task<void> AdvertiseLocal(std::shared_ptr<Preview> model, std::shared_ptr<Bluetooth> bluetooth, ApplicationHandle app,
                          bool associated) {
  auto& local = model->peripheral;
  if (!co_await AuthorizeAdvertising(app)) {
    local.starting_advertisement = false;
    model->status = "Bluetooth advertising permission was denied";
    co_return;
  }
  if (associated && !local.server) {
    local.starting_advertisement = false;
    co_return;
  }
  auto on_end = [weak = std::weak_ptr<Preview>(model)](PeripheralEndEvent event) {
    if (auto current = weak.lock()) {
      current->peripheral.advertisement.reset();
      current->peripheral.advertising = false;
      if (const auto* error = std::get_if<BluetoothError>(&event))
        current->Report(*error);
    }
  };
  AdvertisingOptions options{.service_uuids = {local_service_uuid}};
  auto pending = associated ? bluetooth->StartAdvertisingAsync(*local.server, std::move(options), {}, on_end)
                            : bluetooth->StartAdvertisingAsync(std::move(options), {}, on_end);
  auto started = co_await std::move(pending);
  local.starting_advertisement = false;
  if (started.Succeeded()) {
    local.advertisement.emplace(std::move(started).Value());
    local.advertising = true;
    model->status = "Advertising";
  } else {
    model->Report(started.Error());
  }
}

View PeripheralControls(const std::shared_ptr<Preview>& model, const std::shared_ptr<Bluetooth>& bluetooth,
                        ApplicationHandle app, TaskScope tasks, BluetoothCapabilities capabilities) {
  if (!capabilities.can_use_gatt_server && !capabilities.can_advertise)
    return {};
  auto& local = model->peripheral;
  const bool open = local.server_open;
  const bool advertising = local.advertising;
  const bool starting = local.starting_advertisement;
  const int value = local.value;
  const auto start_advertising = [model, bluetooth, app, tasks](bool associated) {
    auto& local = model->peripheral;
    if (local.advertising || local.starting_advertisement)
      return;
    local.starting_advertisement = true;
    tasks.Launch(AdvertiseLocal(model, bluetooth, app, associated));
  };
  return Column {
    Text("Local BLE", TextRole::Title),
    Text(local_service_uuid),
    Flow {
      capabilities.can_use_gatt_server ? View(Button(open ? "Close server" : "Open server")
          .With(Enabled(!local.opening)).OnClick([model, bluetooth, app, tasks] {
            auto& local = model->peripheral;
            if (local.server) {
              local.server->Stop();
              local.server.reset();
              local.server_open = false;
              local.subscribed = false;
              model->status = "Local GATT server closed";
            } else if (!local.opening) {
              local.opening = true;
              tasks.Launch(OpenLocalServer(model, bluetooth, app));
            }
          })) : View{},
      capabilities.can_advertise && open && !advertising ? View(Button("Advertise service")
          .With(Enabled(!starting)).OnClick([start_advertising] { start_advertising(true); })) : View{},
      capabilities.can_advertise_standalone && !advertising ? View(Button("Advertise only")
          .With(Enabled(!starting)).OnClick([start_advertising] { start_advertising(false); })) : View{},
      advertising ? View(Button("Stop advertising").OnClick([model] {
        auto& local = model->peripheral;
        if (local.advertisement)
          local.advertisement->Stop();
        local.advertisement.reset();
        local.advertising = false;
        model->status = "Advertising stopped";
      })) : View{},
      open ? View(Button("Notify").With(Enabled(local.subscribed)).OnClick([model, tasks] {
        tasks.Launch([model]() -> Task<void> {
          auto& local = model->peripheral;
          if (!local.server)
            co_return;
          const int current = local.value;
          const int next = (current + 1) % 256;
          local.value = next;
          Bytes bytes{static_cast<std::byte>(next)};
          auto result = co_await local.server->NotifyAsync(local_service_uuid, local_value_uuid, std::move(bytes));
          if (!result.Succeeded())
            model->Report(result.Error());
        });
      })) : View{},
    }.With(Spacing(8.0F)),
    open ? View(Text("Local value " + std::to_string(value))) : View{},
  }.With(Spacing(8.0F));
}

void InstallPreview(ApplicationContext& context) {
#if defined(__ANDROID__)
  android::JavaPlatformModuleFactory<std::shared_ptr<AndroidAdvertisingPermission>> factory;
  factory.class_name = "org.huxerui.lib.bluetooth.preview.MainActivity$AdvertisePermissionFactory";
  factory.create = [](PlatformChannel channel) {
    return std::make_shared<AndroidAdvertisingPermission>(std::move(channel));
  };
  context.RegisterPlatformModule<std::shared_ptr<AndroidAdvertisingPermission>>(advertising_permission_module,
                                                                               std::move(factory));
#endif
  auto model = std::make_shared<Preview>();
  try {
    model->selection_file = UseApplication().Directories().data_directory.Child("bluetooth-device.txt");
  } catch (const std::logic_error&) {
    // Some hosts do not provide application storage.
  }
  context.Provide(std::move(model));
}

} // namespace

View App() {
  const auto app = UseApplication();
  const auto tasks = UseTaskScope();
  const auto app_tasks = UseApplicationTaskScope();
  const auto bluetooth = UseService<Bluetooth>();
  const auto model = UseService<Preview>();
  const auto capabilities = bluetooth->Capabilities();
  const auto weak = std::weak_ptr<Preview>(model);
  const bool scanning = model->scanning;
  const bool connecting = model->connecting;
  const bool connected = model->connected;
  const std::optional<Device> selected = model->selected;
  const std::string saved_id = model->saved_id;
  const bool auto_reconnect = model->auto_reconnect;
  const std::vector<DeviceRow>& devices = model->devices;
  const std::shared_ptr<const std::vector<GattService>> services = model->services;
  const AdapterStatus adapter = model->adapter;

  Lifecycle([model, bluetooth, app, tasks, weak] {
    auto watcher = tasks.Launch(bluetooth->WatchAdapterStatusAsync([weak](AdapterStatus value) {
      if (auto current = weak.lock())
        current->adapter = value;
    }));
    auto restore = tasks.Launch(RestoreSelection(model, bluetooth, app, true));
    return [model, watcher, restore] {
      watcher.Cancel();
      restore.Cancel();
      model->StopScan();
      model->Disconnect();
    };
  });

  std::vector<View> attributes;
  const auto generation = model->connection_generation;
  for (const auto& service : *services) {
    attributes.push_back(Text(service.uuid, TextRole::Title));
    for (const auto& characteristic : service.characteristics) {
      attributes.push_back(Button(characteristic.Uuid()).OnClick(
          [model, generation, characteristic, service_uuid = service.uuid] {
            if (model->connection_generation != generation)
              return;
            model->subscription.reset();
            model->subscribed = false;
            model->value = std::string{};
            model->characteristic = characteristic;
            model->characteristic_path = CharacteristicPath{service_uuid, characteristic.Uuid()};
          }));
    }
  }

  const auto scan_button = Button(scanning ? "Stop scan" : "Scan").With(Enabled(!connecting && !connected))
    .OnClick([model, bluetooth, app, tasks, weak, scanning] {
      if (scanning) {
        if (model->scan)
          model->scan->Stop();
        return;
      }
      tasks.Launch([model, bluetooth, app, weak]() -> Task<void> {
        if (!co_await Authorize(app, Permission::BluetoothScan)) {
          model->status = "Bluetooth scan permission was denied";
          co_return;
        }
        ScanOptions options;
        if (!model->filter->text.empty())
          options.advertised_services.push_back(model->filter->text);
        auto result = bluetooth->StartScan(std::move(options), [weak](ScanEvent event) {
          if (auto current = weak.lock())
            current->Receive(std::move(event));
        });
        if (!result.Succeeded()) {
          model->Report(result.Error());
          co_return;
        }
        model->devices = std::vector<DeviceRow>{};
        model->scan.emplace(std::move(result).Value());
        model->scanning = true;
        model->status = "Scanning";
      });
    });

  const auto chooser_button = Button("Select device").With(Enabled(!connecting && !connected))
    .OnClick([model, bluetooth, tasks, app_tasks] {
      SelectDeviceOptions options;
      const auto& filter = model->filter->text;
      if (filter.empty()) {
        model->status = "Enter the service UUID to grant access to its characteristics";
        return;
      }
      options.advertised_services.push_back(filter);
      const auto revision = model->selection_revision;
      auto selection = bluetooth->SelectDevice(std::move(options));
      tasks.Launch([model, app_tasks, revision, selection = std::move(selection)]() mutable -> Task<void> {
        auto result = co_await std::move(selection);
        if (model->selection_revision != revision || model->connecting || model->connected)
          co_return;
        if (result.Succeeded()) {
          SaveSelection(model, app_tasks, std::move(result).Value());
          model->status = "Device selected";
        } else {
          model->Report(result.Error());
        }
      });
    });

  const auto pair_button = Button("Pair").With(Enabled(!connecting && !connected))
      .OnClick([model, bluetooth, app, tasks, selected] {
        tasks.Launch([model, bluetooth, app, selected]() -> Task<void> {
          if (!co_await Authorize(app, Permission::BluetoothConnect)) {
            model->status = "Bluetooth connect permission was denied";
            co_return;
          }
          auto result = co_await bluetooth->PairAsync(*selected);
          if (result.Succeeded())
            model->status = "Device paired";
          else
            model->Report(result.Error());
        });
      });

  const auto discover_button = Button("Discover services").OnClick([model, tasks] {
    const auto generation = model->connection_generation;
    tasks.Launch([model, generation]() -> Task<void> {
      if (!model->session || model->connection_generation != generation)
        co_return;
      auto result = co_await model->session->DiscoverAsync();
      if (model->connection_generation != generation)
        co_return;
      if (result.Succeeded() && model->session) {
        model->ClearAttributes();
        model->services = std::make_shared<const std::vector<GattService>>(std::move(result).Value());
        model->status = "Services discovered";
      } else if (!result.Succeeded()) {
        model->Report(result.Error());
      }
    });
  });

  return ScrollView(Column {
    Text("Bluetooth", TextRole::Title),
    Text(StatusText(adapter)),
    Text(model->status),
    Switch("Auto reconnect", auto_reconnect).OnChanged([model](bool enabled) {
      model->auto_reconnect = enabled;
      if (!enabled && model->recovering) {
        model->Disconnect();
        model->status = "Reconnect canceled";
      }
    }),
    TextField(model->filter)
        .Label("Advertised service UUID")
        .Placeholder("Any service")
        .OnChanged([model](const TextEditingValue& value) { model->filter = value; }),
    Flow {
      capabilities.can_scan ? View(scan_button) : View{},
      capabilities.can_select_device ? View(chooser_button) : View{},
      capabilities.can_pair && selected.has_value() ? View(pair_button) : View{},
      capabilities.can_restore_device && !saved_id.empty() && !selected
          ? View(Button("Restore saved device").OnClick([model, bluetooth, app, tasks] {
              tasks.Launch(RestoreSelection(model, bluetooth, app, false));
            }))
          : View{},
      !saved_id.empty() && !connecting && !connected
          ? View(Button("Forget saved device").OnClick([model, app_tasks] {
              SaveSelection(model, app_tasks, std::nullopt);
              model->status = "Saved device removed";
            }))
          : View{},
      Button(connecting ? "Cancel connection" : connected ? "Disconnect" : "Connect")
          .With(Enabled(selected.has_value() || connecting || connected))
          .OnClick([model, bluetooth, app, tasks] {
            if (model->connected || model->connecting) {
              model->Disconnect();
              model->status = "Disconnected";
              return;
            }
            const std::optional<Device> device = model->selected;
            if (device)
              StartConnection(model, bluetooth, app, tasks, *device);
          }),
      connected ? View(discover_button) : View{},
    }.With(Spacing(8.0F)),
    selected ? View(Text("Selected: " + selected->Name().value_or(selected->Id()))) : View{},
    ForEach(devices, [model, app_tasks, connecting, connected](const DeviceRow& row) {
      return Button(row.label).With(Enabled(!connecting && !connected))
          .OnClick([model, app_tasks, device = row.device] { SaveSelection(model, app_tasks, device); })
          .Key(row.device.Id());
    }),
    Column(std::move(attributes)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    AttributeControls(model, tasks),
    PeripheralControls(model, bluetooth, app, app_tasks, capabilities),
  }.With(Padding(16.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

const Application application{
    App,
    {
        .window = {
            .title = "Bluetooth Preview",
        },
        .application_hooks = {huxerui::bluetooth::Install, InstallPreview},
    }
};
