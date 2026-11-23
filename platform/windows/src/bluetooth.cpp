#include "backend.h"

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <span>
#include <utility>

namespace huxerui::bluetooth::detail {

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Networking::Sockets;
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
  else if (value == 0x800700aa)
    code = Code::Busy;
  else if (value == 0x80072745 || value == 0x80072746 || value == 0x80072749)
    code = Code::Disconnected;
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

using WinError = winrt::Windows::Devices::Bluetooth::BluetoothError;
using ProviderStatus = GattServiceProviderAdvertisementStatus;

BluetoothError ProviderError(WinError error, const char* message) {
  auto code = Code::PlatformFailure;
  switch (error) {
    case WinError::RadioNotAvailable: code = Code::Unavailable; break;
    case WinError::ResourceInUse: code = Code::Busy; break;
    case WinError::DisabledByPolicy:
    case WinError::DisabledByUser:
    case WinError::ConsentRequired: code = Code::PermissionDenied; break;
    case WinError::NotSupported:
    case WinError::TransportNotSupported: code = Code::Unsupported; break;
    default: break;
  }
  return Error(code, message, "Windows BluetoothError", static_cast<int>(error));
}

struct AttDeferral {
  Deferral value{nullptr};

  ~AttDeferral() {
    if (value) {
      try {
        value.Complete();
      } catch (const winrt::hresult_error&) {
        // The peer can disconnect before its request is answered.
      }
    }
  }
};

struct LocalAttribute {
  GattLocalCharacteristic characteristic{nullptr};
  std::optional<winrt::event_token> read;
  std::optional<winrt::event_token> write;
  std::optional<winrt::event_token> subscription;
  bool subscribed = false;
};

class LocalServer final : public std::enable_shared_from_this<LocalServer> {
public:
  LocalServer(TaskScope tasks, BackendEvents events, Token id, std::string uuid)
      : tasks_(std::move(tasks)), events_(std::move(events)), id_(id), uuid_(std::move(uuid)) {}

  ~LocalServer() { Close(); }

  Token Id() const { return id_; }

  static winrt::fire_and_forget Open(std::shared_ptr<LocalServer> server, ServerService definition,
                                     Completion completion) {
    try {
      const auto uuid = winrt::guid{std::string_view(definition.uuid)};
      auto result = co_await GattServiceProvider::CreateAsync(uuid);
      if (server->closed_)
        co_return;
      if (result.Error() != WinError::Success) {
        completion(Reply::Failure(ProviderError(result.Error(), "Windows could not configure the local service")));
        co_return;
      }
      server->provider_ = result.ServiceProvider();
      for (const auto& item : definition.characteristics) {
        WinProperties properties = WinProperties::None;
        if (item.properties.can_read)
          properties |= WinProperties::Read;
        if (item.properties.can_write_with_response)
          properties |= WinProperties::Write;
        if (item.properties.can_notify)
          properties |= WinProperties::Notify;
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(properties);
        auto created = co_await server->provider_.Service().CreateCharacteristicAsync(
            winrt::guid{std::string_view(item.uuid)}, parameters);
        if (server->closed_)
          co_return;
        if (created.Error() != WinError::Success) {
          completion(Reply::Failure(ProviderError(created.Error(), "Windows could not configure the local characteristic")));
          co_return;
        }
        server->Register(item, created.Characteristic());
      }
      completion(Ok());
    } catch (const winrt::hresult_error& error) {
      if (!server->closed_)
        completion(Reply::Failure(WindowsError(error)));
    }
  }

  void Advertise(const Request& request, Completion completion) {
    if (!provider_ || closed_) {
      completion(Fail(Code::Disconnected, "The local GATT server is closed"));
      return;
    }
    if (request.advertisement.local_name || request.advertisement.service_uuids != std::vector<std::string>{uuid_}) {
      completion(Fail(Code::Unsupported, "Windows can advertise only the configured service UUID and system name"));
      return;
    }
    if (advertisement_) {
      completion(Fail(Code::Busy, "A Windows GATT advertisement is already active or stopping"));
      return;
    }
    const auto id = request.peripheral;
    advertisement_ = id;
    advertising_ = std::move(completion);
    try {
      status_listener_ = provider_.AdvertisementStatusChanged(
          [weak = weak_from_this(), tasks = tasks_, id](const GattServiceProvider&,
                                                const GattServiceProviderAdvertisementStatusChangedEventArgs& info) {
        const auto status = info.Status();
        const auto error = info.Error();
        tasks.Post([weak, id, status, error] {
          if (auto server = weak.lock())
            server->AdvertisingStatus(id, status, error);
        });
      });
      GattServiceProviderAdvertisingParameters options;
      options.IsConnectable(true);
      options.IsDiscoverable(true);
      provider_.StartAdvertising(options);
    } catch (const winrt::hresult_error& error) {
      auto failed = std::exchange(advertising_, {});
      ReleaseAdvertisement();
      failed(Reply::Failure(WindowsError(error)));
    }
  }

  void StopAdvertising(Token id) {
    if (id == 0 || advertisement_ != id || stopping_)
      return;
    stopping_ = true;
    advertising_ = {};
    Unpublish();
    try {
      provider_.StopAdvertising();
      const auto status = provider_.AdvertisementStatus();
      if (status == ProviderStatus::Created || status == ProviderStatus::Stopped || status == ProviderStatus::Aborted)
        ReleaseAdvertisement();
    } catch (const winrt::hresult_error&) {
      ReleaseAdvertisement();
    }
  }

  Cancellation Notify(Token id, const Bytes& value, Completion completion) {
    if (publication_.load() == 0) {
      completion(Fail(Code::Unavailable, "The Windows GATT service is not published"));
      return {};
    }
    const auto found = attributes_.find(id);
    if (found == attributes_.end()) {
      completion(Fail(Code::InvalidArgument, "Unknown local GATT characteristic"));
      return {};
    }
    try {
      const auto& characteristic = found->second.characteristic;
      const auto peers = characteristic.SubscribedClients();
      if (peers.Size() == 0) {
        completion(Ok());
        return {};
      }
      for (const auto& peer : peers) {
        if (value.size() > peer.MaxNotificationSize()) {
          completion(Fail(Code::PayloadTooLarge, "The notification exceeds a subscribed peer's update size"));
          return {};
        }
      }
      auto operation = characteristic.NotifyValueAsync(ToBuffer(value));
      FinishNotification(operation, std::move(completion));
      return [operation] {
        try {
          operation.Cancel();
        } catch (const winrt::hresult_error&) {
          // The platform already completed the update.
        }
      };
    } catch (const winrt::hresult_error& error) {
      completion(Reply::Failure(WindowsError(error)));
      return {};
    }
  }

  void Close() {
    if (std::exchange(closed_, true))
      return;
    StopAdvertising(advertisement_);
    ReleaseAdvertisement();
    for (const auto& [id, attribute] : attributes_) {
      if (attribute.read)
        attribute.characteristic.ReadRequested(*attribute.read);
      if (attribute.write)
        attribute.characteristic.WriteRequested(*attribute.write);
      if (attribute.subscription)
        attribute.characteristic.SubscribedClientsChanged(*attribute.subscription);
    }
    attributes_.clear();
    provider_ = nullptr;
  }

private:
  void Register(const ServerCharacteristic& definition, const GattLocalCharacteristic& characteristic) {
    const auto id = definition.id;
    auto& attribute = attributes_.emplace(id, LocalAttribute{characteristic}).first->second;
    if (definition.properties.can_read) {
      attribute.read = characteristic.ReadRequested(
          [weak = weak_from_this(), tasks = tasks_, id](const GattLocalCharacteristic&,
                                                       const GattReadRequestedEventArgs& args) {
        Dispatch(weak, tasks, id, args);
      });
    }
    if (definition.properties.can_write_with_response) {
      attribute.write = characteristic.WriteRequested(
          [weak = weak_from_this(), tasks = tasks_, id](const GattLocalCharacteristic&,
                                                       const GattWriteRequestedEventArgs& args) {
        Dispatch(weak, tasks, id, args);
      });
    }
    if (definition.properties.can_notify) {
      attribute.subscription = characteristic.SubscribedClientsChanged(
          [weak = weak_from_this(), tasks = tasks_, id](const GattLocalCharacteristic& source, const IInspectable&) {
        tasks.Post([weak, id, source] {
          auto server = weak.lock();
          if (!server || server->publication_.load() == 0)
            return;
          try {
            auto& local = server->attributes_.at(id);
            const bool enabled = source.SubscribedClients().Size() != 0;
            if (std::exchange(local.subscribed, enabled) != enabled)
              server->events_(ServerSubscription{server->id_, id, enabled});
          } catch (const winrt::hresult_error&) {
            // The platform already withdrew the characteristic.
          }
        });
      });
    }
  }

  template <class Args>
  static winrt::fire_and_forget Dispatch(std::weak_ptr<LocalServer> weak, TaskScope tasks, Token id, Args args) {
    constexpr bool write = std::is_same_v<Args, GattWriteRequestedEventArgs>;
    try {
      Token publication = 0;
      if (auto server = weak.lock())
        publication = server->publication_.load();
      const auto deferral = std::make_shared<AttDeferral>();
      deferral->value = args.GetDeferral();
      auto request = co_await args.GetRequestAsync();
      if (!request)
        co_return;
      if constexpr (write) {
        if (request.Option() != GattWriteOption::WriteWithResponse)
          co_return;
      }
      tasks.Post([weak, id, request, deferral, publication] {
        auto server = weak.lock();
        try {
          if (!server || publication == 0 || server->publication_.load() != publication) {
            request.RespondWithProtocolError(static_cast<std::uint8_t>(AttStatus::UnlikelyError));
            return;
          }
          Bytes value;
          std::optional<std::size_t> limit;
          if constexpr (write)
            value = BufferBytes(request.Value());
          else
            limit = request.Length();
          auto current = [weak, publication] {
            const auto server = weak.lock();
            return server && server->publication_.load() == publication;
          };
          server->events_(ServerRequest{server->id_, id, write, request.Offset(), std::move(value), limit,
                                         [current, request, deferral](AttStatus status, Bytes response) {
            try {
              if (!current())
                status = AttStatus::UnlikelyError;
              if (status != AttStatus::Success) {
                request.RespondWithProtocolError(static_cast<std::uint8_t>(status));
              } else if constexpr (write) {
                request.Respond();
              } else {
                request.RespondWithValue(ToBuffer(response));
              }
            } catch (const winrt::hresult_error&) {
              // The peer can disconnect before its request is answered.
            }
          }, current});
        } catch (const winrt::hresult_error&) {
          // The platform retired the request before dispatch.
        }
      });
    } catch (const winrt::hresult_error&) {
      // The platform retired the request before it could be acquired.
    }
  }

  static winrt::fire_and_forget FinishNotification(
      IAsyncOperation<winrt::Windows::Foundation::Collections::IVectorView<GattClientNotificationResult>> operation,
      Completion completion) {
    try {
      auto results = co_await operation;
      for (const auto& result : results) {
        if (result.Status() != GattCommunicationStatus::Success) {
          completion(GattResult(result.Status(), result.ProtocolError()));
          co_return;
        }
      }
      completion(Ok());
    } catch (const winrt::hresult_error& error) {
      completion(Reply::Failure(WindowsError(error)));
    }
  }

  void AdvertisingStatus(Token id, ProviderStatus status, WinError error) {
    if (advertisement_ != id)
      return;
    if (stopping_) {
      if (status == ProviderStatus::Stopped || status == ProviderStatus::Aborted)
        ReleaseAdvertisement();
      return;
    }
    if (status == ProviderStatus::Started) {
      publication_.store(id);
      if (auto completion = std::exchange(advertising_, {}))
        completion(Ok());
      return;
    }
    if (status == ProviderStatus::Created)
      return;
    auto failure = status == ProviderStatus::StartedWithoutAllAdvertisementData
                       ? Error(Code::PayloadTooLarge, "Windows omitted requested advertising data")
                       : ProviderError(error, "Windows stopped the GATT advertisement");
    if (auto completion = std::exchange(advertising_, {}))
      completion(Reply::Failure(std::move(failure)));
    else
      events_(PeripheralFailure{id, std::move(failure)});
    StopAdvertising(id);
  }

  void Unpublish() {
    if (publication_.exchange(0) == 0)
      return;
    for (auto& [id, attribute] : attributes_) {
      if (std::exchange(attribute.subscribed, false))
        events_(ServerSubscription{id_, id, false});
    }
  }

  void ReleaseAdvertisement() {
    advertisement_ = 0;
    stopping_ = false;
    if (auto token = std::exchange(status_listener_, {}))
      provider_.AdvertisementStatusChanged(*token);
  }

  TaskScope tasks_;
  BackendEvents events_;
  Token id_;
  std::string uuid_;
  GattServiceProvider provider_{nullptr};
  std::map<Token, LocalAttribute> attributes_;
  std::optional<winrt::event_token> status_listener_;
  Completion advertising_;
  Token advertisement_ = 0;
  std::atomic<Token> publication_{0};
  bool stopping_ = false;
  bool closed_ = false;
};

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

constexpr std::string_view classic_prefix = "rfcomm:";

struct RfcommChannel {
  explicit RfcommChannel(std::string device_id) : device_id(std::move(device_id)) {}

  void Close() {
    retired.store(true);
    try {
      socket.Close();
    } catch (const winrt::hresult_error&) {
      // Windows can close the socket before the application releases it.
    }
    service = nullptr;
    device = nullptr;
  }

  std::string device_id;
  StreamSocket socket{};
  BluetoothDevice device{nullptr};
  RfcommDeviceService service{nullptr};
  std::atomic<bool> retired{false};
  bool connecting = true;
};

winrt::fire_and_forget ListRfcommDevices(Completion reply) {
  try {
    auto devices = co_await DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
    std::vector<FoundDevice> result;
    for (const auto& device : devices)
      result.push_back({std::string(classic_prefix) + winrt::to_string(device.Id()), Name(device.Name())});
    reply(Ok(std::move(result)));
  } catch (const winrt::hresult_error& error) {
    reply(Reply::Failure(WindowsError(error)));
  }
}

winrt::fire_and_forget RestoreRfcommDevice(std::string id, Completion reply) {
  try {
    auto device = co_await BluetoothDevice::FromIdAsync(winrt::to_hstring(id.substr(classic_prefix.size())));
    if (!device || !device.DeviceInformation().Pairing().IsPaired())
      reply(Fail(Code::DeviceUnavailable, "Windows no longer recognizes this paired Classic device"));
    else
      reply(Ok(FoundDevice{std::move(id), Name(device.Name())}));
  } catch (const winrt::hresult_error& error) {
    reply(Reply::Failure(WindowsError(error)));
  }
}

winrt::fire_and_forget OpenRfcommChannel(TaskScope tasks, std::shared_ptr<RfcommChannel> channel,
                                       std::string service_uuid, Completion reply) {
  try {
    const auto socket = channel->socket;
    auto device = co_await BluetoothDevice::FromIdAsync(
        winrt::to_hstring(channel->device_id.substr(classic_prefix.size())));
    if (channel->retired.load())
      co_return;
    if (!device || !device.DeviceInformation().Pairing().IsPaired()) {
      reply(Fail(Code::DeviceUnavailable, "Windows no longer recognizes this paired Classic device"));
      co_return;
    }
    const auto id = RfcommServiceId::FromUuid(winrt::guid{std::string_view(service_uuid)});
    auto services = co_await device.GetRfcommServicesForIdAsync(id, BluetoothCacheMode::Uncached);
    if (channel->retired.load())
      co_return;
    if (services.Error() != WinError::Success) {
      reply(Reply::Failure(ProviderError(services.Error(), "Windows could not resolve the RFCOMM service")));
      co_return;
    }
    if (services.Services().Size() != 1) {
      reply(services.Services().Size() == 0 ?
          Fail(Code::DeviceUnavailable, "The RFCOMM service is unavailable on the peer") :
          Fail(Code::Unsupported, "The RFCOMM UUID resolves to multiple services"));
      co_return;
    }
    auto service = services.Services().GetAt(0);
    co_await socket.ConnectAsync(service.ConnectionHostName(), service.ConnectionServiceName(),
                                 SocketProtectionLevel::BluetoothEncryptionWithAuthentication);
    tasks.Post([channel, device = std::move(device), service = std::move(service), reply = std::move(reply)]() mutable {
      if (!channel->retired.load()) {
        channel->device = std::move(device);
        channel->service = std::move(service);
        channel->connecting = false;
        reply(Ok());
      }
    });
  } catch (const winrt::hresult_error& error) {
    if (!channel->retired.load())
      reply(Reply::Failure(WindowsError(error)));
  }
}

winrt::fire_and_forget RfcommSocketIo(StreamSocket socket, Request request, Completion reply) {
  try {
    if (request.operation == Operation::ChannelRead) {
      auto bytes = co_await socket.InputStream().ReadAsync(Buffer(channel_read_size), channel_read_size,
                                                          InputStreamOptions::Partial);
      reply(Ok(BufferBytes(bytes)));
    } else {
      const auto output = socket.OutputStream();
      std::size_t offset = 0;
      while (offset < request.value.size()) {
        const auto size = std::min(channel_read_size, request.value.size() - offset);
        Buffer bytes(static_cast<std::uint32_t>(size));
        std::memcpy(bytes.data(), request.value.data() + offset, size);
        bytes.Length(static_cast<std::uint32_t>(size));
        const auto written = co_await output.WriteAsync(bytes);
        if (written == 0 || written > size) {
          reply(Fail(Code::Disconnected, "The Windows RFCOMM socket stopped accepting data"));
          co_return;
        }
        offset += written;
      }
      reply(Ok());
    }
  } catch (const winrt::hresult_error& error) {
    reply(Reply::Failure(WindowsError(error)));
  }
}

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
      if (!adapter || (!adapter.IsLowEnergySupported() && !adapter.IsClassicSupported())) {
        if (!state->disposed)
          state->SetStatus(AdapterStatus::Unavailable);
        co_return;
      }
      auto radio = co_await adapter.GetRadioAsync();
      if (state->disposed || !radio)
        co_return;
      state->supports_classic = adapter.IsClassicSupported();
      state->supports_peripheral = adapter.IsLowEnergySupported() && adapter.IsPeripheralRoleSupported() &&
          ApiInformation::IsTypePresent(L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattServiceProvider");
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
      if (request.operation == Operation::ListRfcomm) {
        ListRfcommDevices(std::move(reply));
        return {};
      }
      if (request.operation == Operation::Restore && request.device.starts_with(classic_prefix)) {
        RestoreRfcommDevice(std::move(request.device), std::move(reply));
        return {};
      }
      if (request.operation == Operation::OpenRfcomm) {
        if (!request.device.starts_with(classic_prefix)) {
          reply(Fail(Code::DeviceUnavailable, "The device has no paired Classic identity"));
          return {};
        }
        if (std::ranges::any_of(channels, [&](const auto& entry) {
              return entry.second->connecting && entry.second->device_id == request.device;
            })) {
          reply(Fail(Code::Busy, "An RFCOMM open is already in progress for this device"));
          return {};
        }
        auto channel = std::make_shared<RfcommChannel>(std::move(request.device));
        channels.emplace(request.link, channel);
        OpenRfcommChannel(tasks, std::move(channel), std::move(request.service_uuid), std::move(reply));
        return {};
      }
      if (request.operation == Operation::ChannelRead || request.operation == Operation::ChannelWrite) {
        const auto found = channels.find(request.link);
        if (found == channels.end() || found->second->connecting)
          reply(Fail(Code::Disconnected, "The RFCOMM channel is closed"));
        else
          RfcommSocketIo(found->second->socket, std::move(request), std::move(reply));
        return {};
      }
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

  void CloseChannel(Token id) {
    if (auto channel = channels.extract(id); !channel.empty())
      channel.mapped()->Close();
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
    while (!channels.empty())
      CloseChannel(channels.begin()->first);
  }

  TaskScope tasks;
  BackendEvents events;
  bool disposed = false;
  bool supports_peripheral = false;
  std::optional<bool> supports_classic;
  AdapterStatus status = AdapterStatus::Unknown;
  Radio radio{nullptr};
  std::optional<winrt::event_token> radio_token;
  BluetoothLEAdvertisementWatcher watcher{nullptr};
  winrt::event_token received{};
  winrt::event_token stopped{};
  Token scan = 0;
  Token next = 1;
  std::map<Token, std::shared_ptr<Link>> links;
  std::map<Token, std::shared_ptr<RfcommChannel>> channels;
};

class WindowsBackend final : public Backend {
public:
  WindowsBackend(TaskScope tasks, BackendEvents events)
      : state_(std::make_shared<State>(std::move(tasks), std::move(events))) {
    if (auto caps = PlatformCapabilities(); caps.can_use_gatt_client || caps.can_connect_rfcomm)
      State::Initialize(state_);
    else
      state_->status = AdapterStatus::Unavailable;
  }

  ~WindowsBackend() override {
    if (local_)
      local_->Close();
    state_->Dispose();
  }

  AdapterStatus Status() const override {
    return state_->status;
  }

  BluetoothCapabilities ChannelCapabilities() const override {
    auto capabilities = PlatformCapabilities();
    if (state_->supports_classic == false) {
      capabilities.can_list_paired_rfcomm_devices = false;
      capabilities.can_connect_rfcomm = false;
    }
    return capabilities;
  }

  void CloseChannel(Token id) override {
    state_->CloseChannel(id);
  }

  BluetoothCapabilities PeripheralCapabilities() const override {
    return {.can_advertise = state_->supports_peripheral, .can_use_gatt_server = state_->supports_peripheral};
  }

  Cancellation Invoke(Request request, Completion complete) override {
    if (request.operation == Operation::OpenServer) {
      if (request.local_services.size() != 1) {
        complete(Fail(Code::Unsupported, "The Windows backend supports one local primary service per server"));
      } else if (local_) {
        complete(Fail(Code::Busy, "A Windows GATT server is already open"));
      } else {
        auto service = std::move(request.local_services.front());
        const auto id = request.peripheral;
        local_ = std::make_shared<LocalServer>(state_->tasks, state_->events, id, service.uuid);
        LocalServer::Open(local_, std::move(service), std::move(complete));
        return [this, id] { CloseServer(id); };
      }
      return {};
    }
    if (request.operation == Operation::Advertise) {
      if (!local_ || local_->Id() != request.server) {
        complete(Fail(Code::Disconnected, "The local GATT server is closed"));
        return {};
      }
      auto weak = std::weak_ptr<LocalServer>(local_);
      const auto id = request.peripheral;
      local_->Advertise(request, std::move(complete));
      return [weak, id] {
        if (auto server = weak.lock())
          server->StopAdvertising(id);
      };
    }
    if (request.operation == Operation::SendNotification) {
      if (local_ && local_->Id() == request.peripheral)
        return local_->Notify(request.attribute, request.value, std::move(complete));
      complete(Fail(Code::Disconnected, "The local GATT server is closed"));
      return {};
    }
    return state_->Invoke(std::move(request), std::move(complete));
  }

  void StopAdvertising(Token id) override {
    if (local_)
      local_->StopAdvertising(id);
  }

  void CloseServer(Token id) override {
    if (local_ && local_->Id() == id)
      std::exchange(local_, {})->Close();
  }

  void StopScan(Token scan) override {
    state_->StopScan(scan);
  }

  void Disconnect(Token link) override {
    state_->Disconnect(link);
  }

private:
  std::shared_ptr<State> state_;
  std::shared_ptr<LocalServer> local_;
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
  static const bool rfcomm = [] {
    try {
      return ApiInformation::IsMethodPresent(L"Windows.Devices.Bluetooth.BluetoothDevice",
                                             L"GetRfcommServicesForIdAsync", 2);
    } catch (const winrt::hresult_error&) {
      return false;
    }
  }();
  return {.can_scan = supported, .can_restore_device = supported || rfcomm, .can_pair = supported,
          .can_query_bond = supported, .can_use_gatt_client = supported, .can_access_descriptors = supported,
          .can_list_paired_rfcomm_devices = rfcomm, .can_connect_rfcomm = rfcomm};
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
