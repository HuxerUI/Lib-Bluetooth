#include <huxerui/bluetooth.h>
#include <huxerui/huxerui.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <utility>

using namespace huxerui;
using namespace huxerui::bluetooth;

namespace {

struct DeviceRow {
  Device device;
  std::string label;

  bool operator==(const DeviceRow&) const = default;
};

struct Preview {
  State<AdapterStatus> adapter{AdapterStatus::Unknown};
  State<std::vector<DeviceRow>> devices{std::vector<DeviceRow>{}};
  State<std::optional<Device>> selected{std::optional<Device>{}};
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

  std::optional<ScanSession> scan;
  std::optional<GattSession> session;
  std::optional<GattSubscription> subscription;
  TaskHandle operation;

  void StopScan() {
    scan.reset();
    scanning = false;
  }

  void ClearAttributes() {
    subscription.reset();
    subscribed = false;
    characteristic = std::optional<GattCharacteristic>{};
    services = std::make_shared<const std::vector<GattService>>();
  }

  void Disconnect() {
    operation.Cancel();
    ClearAttributes();
    if (session)
      session->Disconnect();
    session.reset();
    connecting = false;
    connected = false;
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

Task<bool> Authorize(ApplicationHandle app, Permission permission) {
#if defined(__ANDROID__) || defined(__APPLE__)
  auto status = co_await app.CheckPermissionAsync(permission);
  if (status == PermissionStatus::NotDetermined || status == PermissionStatus::Denied)
    status = co_await app.RequestPermissionAsync(permission);
  co_return status == PermissionStatus::Granted || status == PermissionStatus::Limited ||
            status == PermissionStatus::SystemDisabled;
#else
  (void)app;
  (void)permission;
  co_return true;
#endif
}

View AttributeControls(const std::shared_ptr<Preview>& model, TaskScope tasks) {
  const std::optional<GattCharacteristic> characteristic = model->characteristic;
  if (!characteristic || !model->session)
    return {};
  const auto properties = characteristic->Properties();
  const bool subscribed = model->subscribed;
  const auto write = [model, tasks, characteristic](GattWriteMode mode) {
    auto bytes = ParseHex(model->payload->text);
    if (!bytes) {
      model->status = "Enter hexadecimal byte pairs";
      return;
    }
    tasks.Launch([model, characteristic, mode, bytes = std::move(*bytes)]() mutable -> Task<void> {
      if (!model->session)
        co_return;
      auto result = co_await model->session->WriteAsync(*characteristic, std::move(bytes), mode);
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
      Button("Read").With(Enabled(properties.can_read)).OnClick([model, tasks, characteristic] {
        tasks.Launch([model, characteristic]() -> Task<void> {
          if (!model->session)
            co_return;
          auto result = co_await model->session->ReadAsync(*characteristic);
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
          .OnClick([model, tasks, characteristic, subscribed] {
            if (subscribed) {
              model->subscription.reset();
              model->subscribed = false;
              return;
            }
            tasks.Launch([model, characteristic]() -> Task<void> {
              if (!model->session)
                co_return;
              auto result = co_await model->session->SubscribeAsync(*characteristic,
                  [weak = std::weak_ptr<Preview>(model)](Bytes value) {
                    if (auto current = weak.lock())
                      current->value = FormatHex(value);
                  });
              const std::optional<GattCharacteristic> selected = model->characteristic;
              if (!result.Succeeded()) {
                model->Report(result.Error());
              } else if (selected == characteristic) {
                model->subscription.emplace(std::move(result).Value());
                model->subscribed = true;
              }
            });
          }),
    }.With(Spacing(8.0F)),
    Text(model->value),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

void InstallPreview(ApplicationContext& context) {
  context.Provide(std::make_shared<Preview>());
}

} // namespace

View App() {
  const auto app = UseApplication();
  const auto tasks = UseTaskScope();
  const auto bluetooth = UseService<Bluetooth>();
  const auto model = UseService<Preview>();
  const auto capabilities = bluetooth->Capabilities();
  const auto weak = std::weak_ptr<Preview>(model);
  const bool scanning = model->scanning;
  const bool connecting = model->connecting;
  const bool connected = model->connected;
  const std::optional<Device> selected = model->selected;
  const std::vector<DeviceRow>& devices = model->devices;
  const std::shared_ptr<const std::vector<GattService>> services = model->services;
  const AdapterStatus adapter = model->adapter;

  Lifecycle([model, bluetooth, tasks, weak] {
    auto watcher = tasks.Launch(bluetooth->WatchAdapterStatusAsync([weak](AdapterStatus value) {
      if (auto current = weak.lock())
        current->adapter = value;
    }));
    return [model, watcher] {
      watcher.Cancel();
      model->StopScan();
      model->Disconnect();
    };
  });

  std::vector<View> attributes;
  for (const auto& service : *services) {
    attributes.push_back(Text(service.uuid, TextRole::Title));
    for (const auto& characteristic : service.characteristics) {
      attributes.push_back(Button(characteristic.Uuid()).OnClick([model, characteristic] {
        model->subscription.reset();
        model->subscribed = false;
        model->value = std::string{};
        model->characteristic = characteristic;
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
    .OnClick([model, bluetooth, tasks] {
      SelectDeviceOptions options;
      const auto& filter = model->filter->text;
      if (filter.empty()) {
        model->status = "Enter the service UUID to grant access to its characteristics";
        return;
      }
      options.advertised_services.push_back(filter);
      auto selection = bluetooth->SelectDevice(std::move(options));
      tasks.Launch([model, selection = std::move(selection)]() mutable -> Task<void> {
        auto result = co_await std::move(selection);
        if (result.Succeeded()) {
          model->selected = std::move(result).Value();
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
    tasks.Launch([model]() -> Task<void> {
      if (!model->session)
        co_return;
      auto result = co_await model->session->DiscoverAsync();
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
    TextField(model->filter)
        .Label("Advertised service UUID")
        .Placeholder("Any service")
        .OnChanged([model](const TextEditingValue& value) { model->filter = value; }),
    Flow {
      capabilities.can_scan ? View(scan_button) : View{},
      capabilities.can_select_device ? View(chooser_button) : View{},
      capabilities.can_pair && selected.has_value() ? View(pair_button) : View{},
      Button(connecting ? "Cancel connection" : connected ? "Disconnect" : "Connect")
          .With(Enabled(selected.has_value() || connecting || connected))
          .OnClick([model, bluetooth, app, tasks, weak, selected, connecting, connected] {
            if (connected || connecting) {
              model->Disconnect();
              model->status = "Disconnected";
              return;
            }
            if (!selected)
              return;
            model->StopScan();
            model->connecting = true;
            model->status = "Connecting";
            model->operation = tasks.Launch([model, bluetooth, app, weak, selected]() -> Task<void> {
              if (!co_await Authorize(app, Permission::BluetoothConnect)) {
                model->connecting = false;
                model->status = "Bluetooth connect permission was denied";
                co_return;
              }
              auto result = co_await bluetooth->ConnectAsync(*selected, {}, [weak](GattEvent event) {
                if (auto current = weak.lock()) {
                  current->ClearAttributes();
                  if (const auto* closed = std::get_if<GattDisconnected>(&event)) {
                    current->session.reset();
                    current->connected = false;
                    if (closed->error)
                      current->Report(*closed->error);
                    else
                      current->status = "Disconnected";
                  } else {
                    current->status = "Services changed. Discover again.";
                  }
                }
              });
              model->connecting = false;
              if (result.Succeeded()) {
                model->session.emplace(std::move(result).Value());
                model->connected = true;
                model->status = "Connected";
              } else {
                model->Report(result.Error());
              }
            });
          }),
      connected ? View(discover_button) : View{},
    }.With(Spacing(8.0F)),
    selected ? View(Text("Selected: " + selected->Name().value_or(selected->Id()))) : View{},
    ForEach(devices, [model, connecting, connected](const DeviceRow& row) {
      return Button(row.label).With(Enabled(!connecting && !connected))
          .OnClick([model, device = row.device] { model->selected = device; }).Key(row.device.Id());
    }),
    Column(std::move(attributes)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    AttributeControls(model, tasks),
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
