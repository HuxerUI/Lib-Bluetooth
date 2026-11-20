#include "backend.h"

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <span>
#include <utility>

namespace huxerui::bluetooth::detail {

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Storage::Streams;

using Code = BluetoothErrorCode;
using WinSession = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession;
using WinCharacteristic = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic;
using WinDescriptor = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDescriptor;
using WinProperties = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;

std::string Uuid(const winrt::guid& guid) {
  auto text = winrt::to_string(winrt::to_hstring(guid));
  if (text.size() == 38)
    text = text.substr(1, 36);
  return NormalizeUuid(text).value_or(text);
}

std::optional<std::string> Name(const winrt::hstring& value) {
  return value.empty() ? std::nullopt : std::optional(winrt::to_string(value));
}

Bytes BufferBytes(const IBuffer& buffer) {
  if (!buffer || buffer.Length() == 0)
    return {};
  const auto* data = reinterpret_cast<const std::byte*>(buffer.data());
  return {data, data + buffer.Length()};
}

IBuffer ToBuffer(const Bytes& bytes) {
  Buffer buffer(static_cast<std::uint32_t>(bytes.size()));
  if (!bytes.empty())
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
  buffer.Length(static_cast<std::uint32_t>(bytes.size()));
  return buffer;
}

BluetoothError WindowsError(const winrt::hresult_error& error) {
  Code code = Code::PlatformFailure;
  const auto value = static_cast<std::uint32_t>(error.code().value);
  if (value == 0x80070005)
    code = Code::PermissionDenied;
  else if (value == 0x800704c7)
    code = Code::Canceled;
  else if (value == 0x800705b4)
    code = Code::TimedOut;
  return Error(code, winrt::to_string(error.message()), "HRESULT", error.code().value);
}

Reply GattResult(GattCommunicationStatus status, const IReference<std::uint8_t>& protocol = nullptr) {
  switch (status) {
    case GattCommunicationStatus::Success: return Ok();
    case GattCommunicationStatus::Unreachable: return Fail(Code::DeviceUnavailable, "Windows cannot reach the GATT device");
    case GattCommunicationStatus::AccessDenied: return Fail(Code::PermissionDenied, "Access to the GATT attribute was denied");
    default:
      if (protocol) {
        const auto status = protocol.Value();
        auto code = Code::PlatformFailure;
        switch (status) {
          case 0x01: code = Code::StaleAttribute; break;
          case 0x02:
          case 0x03: code = Code::UnsupportedProperty; break;
          case 0x05:
          case 0x08:
          case 0x0c:
          case 0x0f: code = Code::AuthenticationRequired; break;
          case 0x0d: code = Code::PayloadTooLarge; break;
          default: break;
        }
        return Reply::Failure(Error(code, "The peer reported a GATT protocol error", "ATT", status));
      }
      return Reply::Failure(Error(Code::PlatformFailure, "Windows reported a GATT protocol failure",
                                  "GattCommunicationStatus", static_cast<int>(status)));
  }
}

Reply PairResult(DevicePairingResultStatus result) {
  Code code = Code::AuthenticationFailed;
  switch (result) {
    case DevicePairingResultStatus::Paired:
    case DevicePairingResultStatus::AlreadyPaired: return Ok();
    case DevicePairingResultStatus::PairingCanceled: code = Code::Canceled; break;
    case DevicePairingResultStatus::AccessDenied: code = Code::PermissionDenied; break;
    case DevicePairingResultStatus::OperationAlreadyInProgress: code = Code::Busy; break;
    case DevicePairingResultStatus::AuthenticationTimeout: code = Code::TimedOut; break;
    default: break;
  }
  return Reply::Failure(Error(code, "Windows Bluetooth pairing did not complete",
                              "DevicePairingResultStatus", static_cast<int>(result)));
}

struct Address {
  std::uint64_t value;
  std::optional<BluetoothAddressType> type;
};

std::string DeviceId(Address address) {
  const char kind = !address.type ? 'u' : *address.type == BluetoothAddressType::Random ? 'r' : 'p';
  return kind + std::to_string(address.value);
}

std::optional<Address> ParseAddress(std::string_view text) {
  if (text.size() < 2)
    return {};
  const char kind = text.front();
  text.remove_prefix(1);
  std::uint64_t address = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), address);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || address > 0xffffffffffffull)
    return {};
  if (kind == 'u')
    return Address{address, {}};
  if (kind == 'r' || kind == 'p')
    return Address{address, kind == 'r' ? BluetoothAddressType::Random : BluetoothAddressType::Public};
  return {};
}

void ServiceData(Advertisement& output, const BluetoothLEAdvertisement& packet) {
  for (const auto& section : packet.DataSections()) {
    const auto type = section.DataType();
    const std::size_t width = type == 0x16 ? 2 : type == 0x20 ? 4 : type == 0x21 ? 16 : 0;
    if (width == 0)
      continue;
    auto bytes = BufferBytes(section.Data());
    if (bytes.size() < width)
      continue;
    std::string uuid;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < width; ++index) {
      if (width == 16 && (index == 4 || index == 6 || index == 8 || index == 10))
        uuid += '-';
      const auto byte = std::to_integer<unsigned>(bytes[width - index - 1]);
      uuid += hex[byte >> 4u];
      uuid += hex[byte & 15u];
    }
    if (auto canonical = NormalizeUuid(uuid)) {
      bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(width));
      output.service_data.emplace(std::move(*canonical), std::move(bytes));
    }
  }
}

struct Attribute {
  WinCharacteristic characteristic{nullptr};
  WinDescriptor descriptor{nullptr};
  std::optional<winrt::event_token> listener;
};

struct Link {
  BluetoothLEDevice device{nullptr};
  WinSession session{nullptr};
  std::optional<winrt::event_token> connection_listener;
  std::optional<winrt::event_token> service_listener;
  bool alive = true;
  std::vector<GattDeviceService> services;
  std::map<Token, Attribute> attributes;

  ~Link() {
    Close();
  }

  void Close() noexcept {
    if (!alive)
      return;
    alive = false;
    try {
      ClearAttributes();
      if (device) {
        if (connection_listener)
          device.ConnectionStatusChanged(*connection_listener);
        if (service_listener)
          device.GattServicesChanged(*service_listener);
      }
      if (session) {
        session.MaintainConnection(false);
        session.Close();
      }
      if (device)
        device.Close();
    } catch (const winrt::hresult_error&) {
      // System teardown can close the device before the application releases it.
    }
  }

  void ClearAttributes() {
    for (auto& [id, attribute] : attributes)
      if (attribute.listener)
        attribute.characteristic.ValueChanged(*attribute.listener);
    attributes.clear();
    for (auto& service : services)
      service.Close();
    services.clear();
  }
};

struct State : std::enable_shared_from_this<State> {
  State(TaskScope scope, BackendEvents receive) : tasks(std::move(scope)), events(std::move(receive)) {}

  void SetStatus(AdapterStatus value) {
    if (status != value) {
      status = value;
      events(status);
    }
  }

  void UpdateRadio() {
    if (!radio)
      return;
    const auto value = radio.State();
    SetStatus(value == RadioState::On ? AdapterStatus::PoweredOn
               : value == RadioState::Off || value == RadioState::Disabled ? AdapterStatus::PoweredOff
                                                                          : AdapterStatus::Unknown);
  }

  static winrt::fire_and_forget Initialize(std::shared_ptr<State> state) {
    try {
      auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
      if (!adapter || !adapter.IsLowEnergySupported()) {
        if (!state->disposed)
          state->SetStatus(AdapterStatus::Unavailable);
        co_return;
      }
      auto radio = co_await adapter.GetRadioAsync();
      if (state->disposed || !radio)
        co_return;
      state->radio = radio;
      state->radio_token = radio.StateChanged([weak = state->weak_from_this(), tasks = state->tasks](auto&&, auto&&) {
        tasks.Post([weak] {
          if (auto state = weak.lock(); state && !state->disposed) {
            try {
              state->UpdateRadio();
            } catch (const winrt::hresult_error&) {
              state->SetStatus(AdapterStatus::Unknown);
            }
          }
        });
      });
      state->UpdateRadio();
    } catch (const winrt::hresult_error&) {
      if (!state->disposed)
        state->SetStatus(AdapterStatus::Unknown);
    }
  }

  void StopScan(Token id) {
    if (scan != id)
      return;
    scan = 0;
    if (auto old = std::exchange(watcher, nullptr)) {
      try {
        old.Received(received);
        old.Stopped(stopped);
        old.Stop();
      } catch (const winrt::hresult_error&) {
        // The watcher can already be closed by the system.
      }
    }
  }

  void StartScan(Token id, Completion complete) {
    BluetoothLEAdvertisementWatcher current{};
    current.ScanningMode(BluetoothLEScanningMode::Active);
    const bool extended = ApiInformation::IsPropertyPresent(
        L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs", L"BluetoothAddressType");
    received = current.Received([weak = weak_from_this(), tasks = tasks, id, extended](auto&&, const auto& args) {
      try {
        const auto address = args.BluetoothAddress();
        if (address == 0 || (extended && args.IsAnonymous()) || args.RawSignalStrengthInDBm() == -127)
          return;
        Address source{address, extended ? std::optional(args.BluetoothAddressType()) : std::nullopt};
        auto adv = args.Advertisement();
        Advertisement data;
        data.local_name = Name(adv.LocalName());
        data.rssi_dbm = args.RawSignalStrengthInDBm();
        if (extended)
          data.connectable = args.IsConnectable();
        for (const auto& service : adv.ServiceUuids())
          data.service_uuids.push_back(Uuid(service));
        for (const auto& item : adv.ManufacturerData())
          data.manufacturer_data.emplace(item.CompanyId(), BufferBytes(item.Data()));
        ServiceData(data, adv);
        auto device = FoundDevice{DeviceId(source), data.local_name};
        tasks.Post([weak, id, data = std::move(data), device = std::move(device)]() mutable {
          if (auto state = weak.lock(); state && state->scan == id)
            state->events(ScanReport{id, std::move(device), std::move(data)});
        });
      } catch (const winrt::hresult_error&) {
        // An invalid or expired advertisement can be skipped.
      }
    });
    stopped = current.Stopped([weak = weak_from_this(), tasks = tasks, id](auto&&, const auto& args) {
      const auto result = args.Error();
      tasks.Post([weak, id, result] {
        if (auto state = weak.lock(); state && state->scan == id)
          state->events(ScanFailure{id, Error(Code::PlatformFailure, "The Windows BLE watcher stopped",
                                            "BluetoothError", static_cast<int>(result))});
      });
    });
    watcher = current;
    scan = id;
    current.Start();
    complete(Ok());
  }

  static IAsyncOperation<BluetoothLEDevice> OpenDevice(Address address) {
    if (address.type)
      return BluetoothLEDevice::FromBluetoothAddressAsync(address.value, *address.type);
    return BluetoothLEDevice::FromBluetoothAddressAsync(address.value);
  }

  static winrt::fire_and_forget DeviceOperation(std::shared_ptr<State> state, Request request, Completion reply,
                                                 std::shared_ptr<bool> canceled, std::shared_ptr<Link> link) {
    try {
      const auto address = ParseAddress(request.device);
      if (!address) {
        reply(Fail(Code::DeviceUnavailable, "Invalid saved Windows Bluetooth device identifier"));
        co_return;
      }
      auto device = co_await OpenDevice(*address);
      if (state->disposed || *canceled)
        co_return;
      if (!device) {
        reply(Fail(Code::DeviceUnavailable, "Windows no longer recognizes this Bluetooth device"));
        co_return;
      }
      if (request.operation == Operation::Restore) {
        reply(Ok(FoundDevice{request.device, Name(device.Name())}));
      } else if (request.operation == Operation::Bond) {
        reply(Ok(device.DeviceInformation().Pairing().IsPaired() ? BondState::Bonded : BondState::NotBonded));
      } else if (request.operation == Operation::Pair) {
        auto result = co_await device.DeviceInformation().Pairing().PairAsync();
        if (!*canceled)
          reply(PairResult(result.Status()));
      } else {
        if (!link->alive)
          co_return;
        link->device = device;
        WinSession gatt{nullptr};
        if (ApiInformation::IsPropertyPresent(L"Windows.Devices.Bluetooth.BluetoothLEDevice", L"BluetoothDeviceId") &&
            ApiInformation::IsMethodPresent(L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession",
                                            L"FromDeviceIdAsync")) {
          gatt = co_await WinSession::FromDeviceIdAsync(device.BluetoothDeviceId());
          if (!link->alive || *canceled)
            co_return;
          if (gatt) {
            link->session = gatt;
            if (gatt.CanMaintainConnection())
              gatt.MaintainConnection(true);
          }
        }
        auto discovered = co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
        if (!link->alive || *canceled)
          co_return;
        if (discovered.Status() != GattCommunicationStatus::Success) {
          reply(GattResult(discovered.Status(), discovered.ProtocolError()));
          co_return;
        }
        const auto weak = state->weak_from_this();
        const auto tasks = state->tasks;
        const auto id = request.link;
        link->connection_listener = device.ConnectionStatusChanged([weak, tasks, id](const auto& source, auto&&) {
          try {
            if (source.ConnectionStatus() != BluetoothConnectionStatus::Disconnected)
              return;
            tasks.Post([weak, id] {
              if (auto state = weak.lock(); state && state->links.contains(id))
                state->events(LinkLost{id, Error(Code::Disconnected, "Windows reported a GATT disconnect")});
            });
          } catch (const winrt::hresult_error&) {
          }
        });
        link->service_listener = device.GattServicesChanged([weak, tasks, id](auto&&, auto&&) {
          tasks.Post([weak, id] {
            if (auto state = weak.lock(); state && state->links.contains(id))
              state->events(ServicesChanged{id});
          });
        });
        if (device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
          reply(Fail(Code::Disconnected, "Windows disconnected during GATT setup"));
          co_return;
        }
        WriteLimits limits;
        if (gatt && gatt.MaxPduSize() > 3)
          limits.without_response = std::min<std::size_t>(512, gatt.MaxPduSize() - 3);
        limits.with_response = 512;
        reply(Ok(limits));
      }
    } catch (const winrt::hresult_error& error) {
      if (!*canceled)
        reply(Reply::Failure(WindowsError(error)));
    }
  }

  static winrt::fire_and_forget Discover(std::shared_ptr<State> state, std::shared_ptr<Link> link, Token id,
                                        Completion reply) {
    try {
      const auto services = co_await link->device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
      if (!link->alive)
        co_return;
      if (services.Status() != GattCommunicationStatus::Success) {
        reply(GattResult(services.Status(), services.ProtocolError()));
        co_return;
      }
      std::vector<GattDeviceService> new_services;
      std::map<Token, Attribute> attributes;
      Discovery result;
      for (const auto& service : services.Services()) {
        auto chars = co_await service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached);
        if (!link->alive)
          co_return;
        if (chars.Status() != GattCommunicationStatus::Success) {
          reply(GattResult(chars.Status(), chars.ProtocolError()));
          co_return;
        }
        RemoteService output{Uuid(service.Uuid()), {}};
        for (const auto& characteristic : chars.Characteristics()) {
          const auto flags = characteristic.CharacteristicProperties();
          const auto has = [flags](WinProperties property) {
            return (flags & property) == property;
          };
          const auto token = state->next++;
          attributes.emplace(token, Attribute{.characteristic = characteristic});
          RemoteCharacteristic attr{token, Uuid(characteristic.Uuid()),
                                    {has(WinProperties::Read), has(WinProperties::Write),
                                     has(WinProperties::WriteWithoutResponse), has(WinProperties::Notify),
                                     has(WinProperties::Indicate)}, {}};
          auto descriptors = co_await characteristic.GetDescriptorsAsync(BluetoothCacheMode::Uncached);
          if (!link->alive)
            co_return;
          if (descriptors.Status() == GattCommunicationStatus::Success) {
            attr.descriptors.emplace();
            for (const auto& descriptor : descriptors.Descriptors()) {
              const auto child = state->next++;
              attributes.emplace(child, Attribute{.descriptor = descriptor});
              attr.descriptors->push_back({child, Uuid(descriptor.Uuid())});
            }
          }
          output.characteristics.push_back(std::move(attr));
        }
        result.services.push_back(std::move(output));
        new_services.push_back(service);
      }
      for (auto& [token, attribute] : link->attributes) {
        if (!attribute.listener)
          continue;
        auto status = co_await attribute.characteristic.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
            GattClientCharacteristicConfigurationDescriptorValue::None);
        if (!link->alive)
          co_return;
        if (status.Status() != GattCommunicationStatus::Success) {
          state->events(ServicesChanged{id});
          reply(GattResult(status.Status(), status.ProtocolError()));
          co_return;
        }
      }
      link->ClearAttributes();
      link->services = std::move(new_services);
      link->attributes = std::move(attributes);
      reply(Ok(std::move(result)));
    } catch (const winrt::hresult_error& error) {
      if (link->alive)
        reply(Reply::Failure(WindowsError(error)));
    }
  }

  static winrt::fire_and_forget AttributeOperation(std::shared_ptr<State> state, std::shared_ptr<Link> link,
                                                  Request request, Completion reply) {
    try {
      auto found = link->attributes.find(request.attribute);
      if (found == link->attributes.end()) {
        reply(Fail(Code::StaleAttribute, "The Windows GATT attribute is stale"));
        co_return;
      }
      auto characteristic = found->second.characteristic;
      auto descriptor = found->second.descriptor;
      if (request.operation == Operation::Read) {
        auto operation = descriptor ? descriptor.ReadValueAsync(BluetoothCacheMode::Uncached)
                                    : characteristic.ReadValueAsync(BluetoothCacheMode::Uncached);
        auto result = co_await operation;
        if (link->alive)
          reply(result.Status() == GattCommunicationStatus::Success ? Ok(BufferBytes(result.Value()))
                                                                  : GattResult(result.Status(), result.ProtocolError()));
      } else if (request.operation == Operation::Write) {
        auto value = ToBuffer(request.value);
        auto operation = descriptor ? descriptor.WriteValueWithResultAsync(value) : characteristic.WriteValueWithResultAsync(
            value, request.write_mode == GattWriteMode::WithResponse ? GattWriteOption::WriteWithResponse
                                                                    : GattWriteOption::WriteWithoutResponse);
        auto result = co_await operation;
        if (link->alive)
          reply(GattResult(result.Status(), result.ProtocolError()));
      } else {
        auto mode = GattClientCharacteristicConfigurationDescriptorValue::None;
        std::optional<winrt::event_token> token;
        if (request.enabled) {
          const auto flags = characteristic.CharacteristicProperties();
          mode = (flags & WinProperties::Notify) == WinProperties::Notify
              ? GattClientCharacteristicConfigurationDescriptorValue::Notify
              : GattClientCharacteristicConfigurationDescriptorValue::Indicate;
          token = characteristic.ValueChanged([weak = state->weak_from_this(), tasks = state->tasks,
                                               id = request.link, attribute = request.attribute](auto&&, auto&& args) {
            try {
              auto value = BufferBytes(args.CharacteristicValue());
              tasks.Post([weak, id, attribute, value = std::move(value)]() mutable {
                if (auto state = weak.lock(); state && state->links.contains(id))
                  state->events(ValueChanged{id, attribute, std::move(value)});
              });
            } catch (const winrt::hresult_error&) {
            }
          });
          found->second.listener = token;
        }
        auto result = co_await characteristic.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(mode);
        if (!link->alive)
          co_return;
        if ((!request.enabled && result.Status() == GattCommunicationStatus::Success) ||
            (request.enabled && result.Status() != GattCommunicationStatus::Success)) {
          if (auto listener = std::exchange(found->second.listener, {}))
            characteristic.ValueChanged(*listener);
        }
        reply(GattResult(result.Status(), result.ProtocolError()));
      }
    } catch (const winrt::hresult_error& error) {
      if (link->alive)
        reply(Reply::Failure(WindowsError(error)));
    }
  }

  Cancellation Invoke(Request request, Completion reply) {
    try {
      if (request.operation == Operation::Scan) {
        StartScan(request.scan, reply);
        return {};
      }
      if (request.operation == Operation::Connect || request.operation == Operation::Pair ||
          request.operation == Operation::Bond || request.operation == Operation::Restore) {
        auto canceled = std::make_shared<bool>(false);
        std::shared_ptr<Link> link;
        if (request.operation == Operation::Connect) {
          link = std::make_shared<Link>();
          links.emplace(request.link, link);
        }
        DeviceOperation(shared_from_this(), std::move(request), std::move(reply), canceled, std::move(link));
        return [canceled] { *canceled = true; };
      }
      const auto found = links.find(request.link);
      if (found == links.end()) {
        reply(Fail(Code::Disconnected, "The Windows GATT session is closed"));
      } else if (request.operation == Operation::Discover) {
        Discover(shared_from_this(), found->second, request.link, std::move(reply));
      } else {
        AttributeOperation(shared_from_this(), found->second, std::move(request), std::move(reply));
      }
    } catch (const winrt::hresult_error& error) {
      reply(Reply::Failure(WindowsError(error)));
    }
    return {};
  }

  void Disconnect(Token id) {
    auto entry = links.extract(id);
    if (!entry.empty())
      entry.mapped()->Close();
  }

  void Dispose() {
    disposed = true;
    StopScan(scan);
    if (radio && radio_token) {
      try {
        radio.StateChanged(*radio_token);
      } catch (const winrt::hresult_error&) {
      }
    }
    while (!links.empty())
      Disconnect(links.begin()->first);
  }

  TaskScope tasks;
  BackendEvents events;
  bool disposed = false;
  AdapterStatus status = AdapterStatus::Unknown;
  Radio radio{nullptr};
  std::optional<winrt::event_token> radio_token;
  BluetoothLEAdvertisementWatcher watcher{nullptr};
  winrt::event_token received{};
  winrt::event_token stopped{};
  Token scan = 0;
  Token next = 1;
  std::map<Token, std::shared_ptr<Link>> links;
};

class WindowsBackend final : public Backend {
public:
  WindowsBackend(TaskScope tasks, BackendEvents events)
      : state_(std::make_shared<State>(std::move(tasks), std::move(events))) {
    if (PlatformCapabilities().can_use_gatt_client)
      State::Initialize(state_);
    else
      state_->status = AdapterStatus::Unavailable;
  }

  ~WindowsBackend() override {
    state_->Dispose();
  }

  AdapterStatus Status() const override {
    return state_->status;
  }

  Cancellation Invoke(Request request, Completion complete) override {
    return state_->Invoke(std::move(request), std::move(complete));
  }

  void StopScan(Token scan) override {
    state_->StopScan(scan);
  }

  void Disconnect(Token link) override {
    state_->Disconnect(link);
  }

private:
  std::shared_ptr<State> state_;
};

} // namespace

BluetoothCapabilities PlatformCapabilities() {
  static const bool supported = [] {
    try {
      return ApiInformation::IsMethodPresent(L"Windows.Devices.Bluetooth.BluetoothLEDevice", L"GetGattServicesAsync");
    } catch (const winrt::hresult_error&) {
      return false;
    }
  }();
  return {.can_scan = supported, .can_restore_device = supported, .can_pair = supported, .can_query_bond = supported,
          .can_use_gatt_client = supported, .can_access_descriptors = supported};
}

void RegisterPlatform(ApplicationContext&) {}

std::unique_ptr<Backend> CreateBackend(TaskScope tasks, BackendEvents events) {
  return std::make_unique<WindowsBackend>(std::move(tasks), std::move(events));
}

} // namespace huxerui::bluetooth::detail

#else

namespace huxerui::bluetooth::detail {

BluetoothCapabilities PlatformCapabilities() {
  return {};
}

void RegisterPlatform(ApplicationContext&) {}

std::unique_ptr<Backend> CreateBackend(TaskScope, BackendEvents) {
  return {};
}

} // namespace huxerui::bluetooth::detail

#endif
