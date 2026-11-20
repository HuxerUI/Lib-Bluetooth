#include "backend.h"

#include <huxerui/android/platform_registry.h>

#include <utility>

namespace huxerui::bluetooth::detail {

namespace {

constexpr char module_name[] = "bluetooth/Android";
using Code = BluetoothErrorCode;

struct BluetoothEventPayload {
  PlatformPayload value;

  static BluetoothEventPayload Decode(const PlatformPayload& payload) {
    return {payload};
  }
};

struct BluetoothEvent : Event<void(BluetoothEventPayload)> {
  static constexpr char Name[] = "bluetooth";
};

const char* Method(Operation operation) {
  switch (operation) {
    case Operation::Scan: return "scan";
    case Operation::Select: return "select";
    case Operation::Restore: return "restore";
    case Operation::Bond: return "bond";
    case Operation::Pair: return "pair";
    case Operation::Connect: return "connect";
    case Operation::Discover: return "discover";
    case Operation::Read: return "read";
    case Operation::Write: return "write";
    case Operation::Notify: return "notify";
  }
  throw std::logic_error("Unknown Android Bluetooth operation");
}

PlatformPayload Encode(const Request& request) {
  PlatformPayload::List services;
  for (const auto& uuid : request.advertised_services)
    services.emplace_back(uuid);
  return PlatformPayload::Object{
      {"link", request.link}, {"attribute", request.attribute}, {"scan", request.scan},
      {"device", request.device}, {"services", std::move(services)}, {"descriptor", request.descriptor},
      {"enabled", request.enabled}, {"response", request.write_mode == GattWriteMode::WithResponse},
      {"value", request.value}};
}

std::optional<std::string> ReadText(const PlatformPayload& value) {
  return value.IsNull() ? std::nullopt : std::optional(std::string(value.AsString()));
}

Bytes ReadBytes(const PlatformPayload& value) {
  const auto bytes = value.AsBytes();
  return {bytes.begin(), bytes.end()};
}

FoundDevice ReadDevice(const PlatformPayload& value) {
  const auto& object = value.AsObject();
  return {std::string(object.at("id").AsString()), ReadText(object.at("name"))};
}

WriteLimits ReadLimits(const PlatformPayload& value) {
  return {static_cast<std::size_t>(value.AsObject().at("response").AsInteger()),
          static_cast<std::size_t>(value.AsObject().at("command").AsInteger())};
}

Discovery ReadDiscovery(const PlatformPayload& value) {
  Discovery discovery;
  for (const auto& entry : value.AsList()) {
    const auto& service = entry.AsObject();
    RemoteService output{std::string(service.at("uuid").AsString()), {}};
    for (const auto& field : service.at("characteristics").AsList()) {
      const auto& characteristic = field.AsObject();
      const auto props = characteristic.at("properties").AsInteger();
      RemoteCharacteristic result{
          static_cast<Token>(characteristic.at("id").AsInteger()), std::string(characteristic.at("uuid").AsString()),
          {(props & 2) != 0, (props & 8) != 0, (props & 4) != 0, (props & 16) != 0, (props & 32) != 0},
          std::vector<RemoteDescriptor>{}};
      for (const auto& item : characteristic.at("descriptors").AsList()) {
        const auto& descriptor = item.AsObject();
        result.descriptors->push_back({static_cast<Token>(descriptor.at("id").AsInteger()),
                                        std::string(descriptor.at("uuid").AsString())});
      }
      output.characteristics.push_back(std::move(result));
    }
    discovery.services.push_back(std::move(output));
  }
  return discovery;
}

BluetoothError AndroidError(std::string_view name, std::string message, const PlatformPayload& detail) {
  Code code = Code::PlatformFailure;
  if (name == "bluetooth/permission")
    code = Code::PermissionDenied;
  else if (name == "bluetooth/unavailable" || name == "huxerui/closed")
    code = Code::Unavailable;
  else if (name == "bluetooth/off")
    code = Code::AdapterOff;
  else if (name == "bluetooth/device")
    code = Code::DeviceUnavailable;
  else if (name == "bluetooth/unsupported")
    code = Code::Unsupported;
  else if (name == "bluetooth/authentication")
    code = Code::AuthenticationRequired;
  else if (name == "bluetooth/pairing")
    code = Code::AuthenticationFailed;
  else if (name == "bluetooth/disconnected")
    code = Code::Disconnected;
  else if (name == "bluetooth/stale")
    code = Code::StaleAttribute;
  else if (name == "bluetooth/busy")
    code = Code::Busy;
  else if (name == "bluetooth/length")
    code = Code::PayloadTooLarge;
  else if (name == "bluetooth/property")
    code = Code::UnsupportedProperty;
  return Error(code, std::move(message), std::string(name),
               detail.Kind() == PlatformPayloadKind::Integer ? std::optional(detail.AsInteger()) : std::nullopt);
}

Reply Decode(Operation operation, const PlatformPayload& value) {
  switch (operation) {
    case Operation::Restore: return Ok(ReadDevice(value));
    case Operation::Bond: return Ok(static_cast<BondState>(value.AsInteger()));
    case Operation::Connect: return Ok(ReadLimits(value));
    case Operation::Discover: return Ok(ReadDiscovery(value));
    case Operation::Read: return Ok(ReadBytes(value));
    default: return Ok();
  }
}

struct Bridge {
  AdapterStatus status = AdapterStatus::Unknown;
  BackendEvents events;

  void Receive(const PlatformPayload& value) {
    const auto& object = value.AsObject();
    const auto type = object.at("type").AsString();
    const auto token = static_cast<Token>(object.at("token").AsInteger());
    const auto& data = object.at("data");
    if (type == "status") {
      status = static_cast<AdapterStatus>(data.AsInteger());
      events(status);
    } else if (type == "scan") {
      const auto& fields = data.AsObject();
      Advertisement advertisement;
      advertisement.local_name = ReadText(fields.at("local_name"));
      for (const auto& service : fields.at("services").AsList())
        advertisement.service_uuids.emplace_back(service.AsString());
      for (const auto& item : fields.at("manufacturer").AsList()) {
        const auto& manufacturer = item.AsObject();
        advertisement.manufacturer_data.emplace(static_cast<std::uint16_t>(manufacturer.at("id").AsInteger()),
                                                ReadBytes(manufacturer.at("value")));
      }
      for (const auto& [uuid, bytes] : fields.at("service_data").AsObject())
        advertisement.service_data.emplace(uuid, ReadBytes(bytes));
      if (!fields.at("rssi").IsNull())
        advertisement.rssi_dbm = static_cast<int>(fields.at("rssi").AsInteger());
      if (!fields.at("connectable").IsNull())
        advertisement.connectable = fields.at("connectable").AsBoolean();
      events(ScanReport{token, ReadDevice(fields.at("device")), std::move(advertisement)});
    } else if (type == "scan_error" || type == "link_lost") {
      const auto& fields = data.AsObject();
      auto error = AndroidError(fields.at("code").AsString(), std::string(fields.at("message").AsString()),
                                fields.at("status"));
      if (type == "scan_error")
        events(ScanFailure{token, std::move(error)});
      else
        events(LinkLost{token, std::move(error)});
    } else if (type == "services") {
      events(ServicesChanged{token});
    } else if (type == "limits") {
      events(LimitsChanged{token, ReadLimits(data)});
    } else if (type == "value") {
      const auto& fields = data.AsObject();
      events(ValueChanged{token, static_cast<Token>(fields.at("attribute").AsInteger()), ReadBytes(fields.at("bytes"))});
    }
  }
};

class AndroidBackend final : public Backend {
public:
  AndroidBackend(PlatformChannel channel, BackendEvents events)
      : channel_(std::move(channel)), bridge_(std::make_shared<Bridge>(Bridge{.events = std::move(events)})) {
    channel_.On<BluetoothEvent>([weak = std::weak_ptr<Bridge>(bridge_)](BluetoothEventPayload event) {
      if (auto bridge = weak.lock())
        bridge->Receive(event.value);
    });
    channel_.Invoke("status", {}, [weak = std::weak_ptr<Bridge>(bridge_)](PlatformResult<PlatformPayload> result) {
      if (auto bridge = weak.lock()) {
        if (const auto* value = std::get_if<PlatformPayload>(&result)) {
          bridge->status = static_cast<AdapterStatus>(value->AsInteger());
          bridge->events(bridge->status);
        }
      }
    });
  }

  ~AndroidBackend() override {
    bridge_.reset();
    channel_.Close();
  }

  AdapterStatus Status() const override {
    return bridge_->status;
  }

  Cancellation Invoke(Request request, Completion completion) override {
    const auto ticket = channel_.Invoke(Method(request.operation), Encode(request),
                                        [operation = request.operation, reply = std::move(completion)]
                                        (PlatformResult<PlatformPayload> answer) {
      if (const auto* error = std::get_if<PlatformError>(&answer)) {
        reply(Reply::Failure(AndroidError(error->code, error->message, error->details)));
      } else {
        try {
          reply(Decode(operation, std::get<PlatformPayload>(answer)));
        } catch (const std::exception& error) {
          reply(Fail(Code::PlatformFailure, error.what()));
        }
      }
    });
    return [channel = channel_, ticket] { (void)channel.Cancel(ticket); };
  }

  void StopScan(Token scan) override {
    Control("stop_scan", scan);
  }

  void Disconnect(Token link) override {
    Control("disconnect", link);
  }

private:
  void Control(const char* method, Token token) {
    channel_.Invoke(method, token, [](PlatformResult<PlatformPayload>) {});
  }

  PlatformChannel channel_;
  std::shared_ptr<Bridge> bridge_;
};

using OpenAndroidModule = std::function<std::unique_ptr<Backend>(BackendEvents)>;

} // namespace

BluetoothCapabilities PlatformCapabilities() {
  return {.can_scan = true, .can_restore_device = true, .can_pair = true, .can_query_bond = true,
          .can_use_gatt_client = true, .can_access_descriptors = true};
}

void RegisterPlatform(ApplicationContext& context) {
  android::JavaPlatformModuleFactory<OpenAndroidModule> factory;
  factory.class_name = "org.huxerui.lib.bluetooth.BluetoothModule$Factory";
  factory.create = [](PlatformChannel channel) -> OpenAndroidModule {
    return [channel = std::move(channel)](BackendEvents events) mutable {
      return std::make_unique<AndroidBackend>(std::move(channel), std::move(events));
    };
  };
  context.RegisterPlatformModule<OpenAndroidModule>(module_name, std::move(factory));
}

std::unique_ptr<Backend> CreateBackend(TaskScope, BackendEvents events) {
  return OpenPlatformModule<OpenAndroidModule>(module_name)(std::move(events));
}

} // namespace huxerui::bluetooth::detail
