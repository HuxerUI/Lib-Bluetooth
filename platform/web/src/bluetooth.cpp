#include "backend.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <utility>

namespace huxerui::bluetooth::detail {

namespace {

using emscripten::val;
using Code = BluetoothErrorCode;

bool HasValue(const val& value) {
  return !value.isNull() && !value.isUndefined();
}

val BrowserBluetooth() {
  const auto navigator = val::global("navigator");
  return HasValue(navigator) ? navigator["bluetooth"] : val::undefined();
}

val Strings(const std::vector<std::string>& values) {
  auto array = val::array();
  for (std::size_t index = 0; index < values.size(); ++index)
    array.set(index, values[index]);
  return array;
}

Bytes ReadBytes(const val& value) {
  auto data = emscripten::convertJSArrayToNumberVector<std::uint8_t>(value);
  Bytes result(data.size());
  for (std::size_t index = 0; index < data.size(); ++index)
    result[index] = std::byte(data[index]);
  return result;
}

FoundDevice ReadDevice(const val& device) {
  const auto name = device["name"];
  return {device["id"].as<std::string>(), HasValue(name) ? std::optional(name.as<std::string>()) : std::nullopt};
}

Discovery ReadDiscovery(const val& value) {
  Discovery discovery;
  for (unsigned s = 0; s < value["length"].as<unsigned>(); ++s) {
    const auto service = value[s];
    RemoteService output{service["uuid"].as<std::string>(), {}};
    const auto characteristics = service["characteristics"];
    for (unsigned c = 0; c < characteristics["length"].as<unsigned>(); ++c) {
      const auto characteristic = characteristics[c];
      const auto props = characteristic["properties"];
      RemoteCharacteristic result{
          characteristic["id"].as<Token>(), characteristic["uuid"].as<std::string>(),
          {props["read"].as<bool>(), props["write"].as<bool>(), props["writeWithoutResponse"].as<bool>(),
           props["notify"].as<bool>(), props["indicate"].as<bool>()}, {}};
      const auto descriptors = characteristic["descriptors"];
      if (HasValue(descriptors)) {
        result.descriptors.emplace();
        for (unsigned d = 0; d < descriptors["length"].as<unsigned>(); ++d) {
          const auto descriptor = descriptors[d];
          result.descriptors->push_back({descriptor["id"].as<Token>(), descriptor["uuid"].as<std::string>()});
        }
      }
      output.characteristics.push_back(std::move(result));
    }
    discovery.services.push_back(std::move(output));
  }
  return discovery;
}

BluetoothError BrowserError(const val& error, Operation operation) {
  const auto name = error["name"].as<std::string>();
  const auto message = error["message"].as<std::string>();
  Code code = Code::PlatformFailure;
  if (name == "SecurityError" || name == "NotAllowedError")
    code = Code::PermissionDenied;
  else if (name == "NotSupportedError")
    code = Code::Unsupported;
  else if (name == "AbortError")
    code = Code::Canceled;
  else if (name == "NotFoundError")
    code = Code::DeviceUnavailable;
  else if (name == "InvalidStateError")
    code = Code::StaleAttribute;
  else if (name == "NetworkError" && operation == Operation::Connect)
    code = Code::DeviceUnavailable;
  else if (name == "InvalidModificationError" && operation == Operation::Write)
    code = Code::PayloadTooLarge;
  return Error(code, message, name);
}

class WebBackend;
std::map<Token, WebBackend*> backends;
Token next_backend = 1;

class WebBackend final : public Backend {
public:
  explicit WebBackend(BackendEvents events)
      : events_(std::move(events)), id_(next_backend++),
        browser_(val::module_property("huxeruiCreateBluetooth")(id_)) {
    backends.emplace(id_, this);
  }

  ~WebBackend() override {
    backends.erase(id_);
    browser_.call<void>("dispose");
  }

  AdapterStatus Status() const override {
    return HasValue(BrowserBluetooth()) ? AdapterStatus::Unknown : AdapterStatus::Unavailable;
  }

  Cancellation Invoke(Request request, Completion completion) override {
    auto parameters = val::object();
    std::string method;
    switch (request.operation) {
      case Operation::Select:
        method = "select";
        parameters.set("services", Strings(request.advertised_services));
        parameters.set("optionalServices", Strings(request.optional_services));
        break;
      case Operation::Restore: method = "restore"; break;
      case Operation::Connect: method = "connect"; break;
      case Operation::Discover: method = "discover"; break;
      case Operation::Read: method = "read"; break;
      case Operation::Write: method = "write"; break;
      case Operation::Notify: method = "notify"; break;
      default:
        completion(Fail(Code::Unsupported, "The browser does not support this Bluetooth operation"));
        return {};
    }
    parameters.set("device", request.device);
    parameters.set("link", request.link);
    parameters.set("attribute", request.attribute);
    parameters.set("descriptor", request.descriptor);
    parameters.set("enabled", request.enabled);
    parameters.set("withResponse", request.write_mode == GattWriteMode::WithResponse);
    if (request.operation == Operation::Write) {
      const auto view = val(emscripten::typed_memory_view(
          request.value.size(), reinterpret_cast<const std::uint8_t*>(request.value.data())));
      parameters.set("bytes", val::global("Uint8Array").new_(view));
    }

    const auto ticket = next_++;
    pending_.emplace(ticket, std::pair{request.operation, std::move(completion)});
    browser_.call<void>("invoke", ticket, method, parameters);
    return [this, ticket] { pending_.erase(ticket); };
  }

  void StopScan(Token) override {}

  void Disconnect(Token link) override {
    browser_.call<void>("disconnect", link);
  }

  void Complete(Token ticket, bool success, const val& value) {
    auto entry = pending_.extract(ticket);
    if (entry.empty())
      return;
    auto& [operation, reply] = entry.mapped();
    try {
      if (!success) {
        reply(Reply::Failure(BrowserError(value, operation)));
        return;
      }
      switch (operation) {
        case Operation::Select:
        case Operation::Restore: reply(Ok(ReadDevice(value))); break;
        case Operation::Discover: reply(Ok(ReadDiscovery(value))); break;
        case Operation::Read: reply(Ok(ReadBytes(value))); break;
        default: reply(Ok()); break;
      }
    } catch (const std::exception& error) {
      reply(Fail(Code::PlatformFailure, error.what()));
    }
  }

  void Event(std::string name, Token link, Token attribute, const val& value) {
    if (name == "disconnect")
      events_(LinkLost{link, Error(Code::Disconnected, "The browser reported a GATT disconnect")});
    else if (name == "services")
      events_(ServicesChanged{link});
    else if (name == "value")
      events_(ValueChanged{link, attribute, ReadBytes(value)});
  }

private:
  BackendEvents events_;
  Token id_;
  val browser_;
  Token next_ = 1;
  std::map<Token, std::pair<Operation, Completion>> pending_;
};

void Complete(Token backend, Token ticket, bool success, val value) {
  if (const auto found = backends.find(backend); found != backends.end())
    found->second->Complete(ticket, success, value);
}

void Event(Token backend, std::string name, Token link, Token attribute, val value) {
  if (const auto found = backends.find(backend); found != backends.end())
    found->second->Event(std::move(name), link, attribute, value);
}

} // namespace

EMSCRIPTEN_BINDINGS(huxerui_bluetooth) {
  emscripten::function("huxeruiBluetoothReply", &Complete);
  emscripten::function("huxeruiBluetoothEvent", &Event);
}

BluetoothCapabilities PlatformCapabilities() {
  const auto browser = BrowserBluetooth();
  if (!HasValue(browser))
    return {};
  return {.can_select_device = HasValue(browser["requestDevice"]), .can_restore_device = HasValue(browser["getDevices"]),
          .can_use_gatt_client = true, .can_access_descriptors = true};
}

void RegisterPlatform(ApplicationContext&) {}

std::unique_ptr<Backend> CreateBackend(TaskScope, BackendEvents events) {
  return std::make_unique<WebBackend>(std::move(events));
}

} // namespace huxerui::bluetooth::detail
