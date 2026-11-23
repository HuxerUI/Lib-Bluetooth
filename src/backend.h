#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <variant>

#include <huxerui/bluetooth.h>

namespace huxerui::bluetooth::detail {

using Token = std::uint32_t;

struct FoundDevice {
  std::string id;
  std::optional<std::string> name;
};

struct RemoteDescriptor {
  Token id;
  std::string uuid;
};

struct RemoteCharacteristic {
  Token id;
  std::string uuid;
  GattCharacteristicProperties properties;
  std::optional<std::vector<RemoteDescriptor>> descriptors;
};

struct RemoteService {
  std::string uuid;
  std::vector<RemoteCharacteristic> characteristics;
};

struct WriteLimits {
  std::optional<std::size_t> with_response;
  std::optional<std::size_t> without_response;
};

struct Discovery {
  std::vector<RemoteService> services;
  Token snapshot = 0;
};

enum class Operation { Scan, Select, Restore, Bond, Pair, Connect, Discover, Read, Write, Notify,
                       Advertise, OpenServer, SendNotification,
                       ListRfcomm, OpenRfcomm, OpenL2cap, ChannelRead, ChannelWrite };

inline bool IsChannelOpen(Operation operation) {
  return operation == Operation::OpenRfcomm || operation == Operation::OpenL2cap;
}

inline constexpr std::size_t channel_read_size = 65535;

inline bool IsPeripheralOperation(Operation operation) {
  return operation == Operation::Advertise || operation == Operation::OpenServer ||
         operation == Operation::SendNotification;
}

struct ServerCharacteristic {
  Token id;
  std::string uuid;
  GattCharacteristicProperties properties;
};

struct ServerService {
  std::string uuid;
  std::vector<ServerCharacteristic> characteristics;
};

struct Request {
  Operation operation;
  Token link = 0;
  Token attribute = 0;
  Token scan = 0;
  Token snapshot = 0;
  bool descriptor = false;
  bool enabled = false;
  GattWriteMode write_mode = GattWriteMode::WithResponse;
  std::string device;
  std::vector<std::string> advertised_services;
  std::vector<std::string> optional_services;
  Bytes value;
  Token peripheral = 0;
  Token server = 0;
  AdvertisingOptions advertisement;
  std::vector<ServerService> local_services;
  std::string service_uuid;
  std::uint16_t psm = 0;
};

using ReplyValue = std::variant<std::monostate, FoundDevice, BondState, WriteLimits, Discovery, Bytes,
                                std::vector<FoundDevice>>;
using Reply = BluetoothResult<ReplyValue>;
using Completion = std::function<void(Reply)>;
using Cancellation = std::function<void()>;

struct ScanReport {
  Token scan;
  FoundDevice device;
  Advertisement advertisement;
};

struct ScanFailure {
  Token scan;
  BluetoothError error;
};

struct LinkLost {
  Token link;
  BluetoothError error;
};

struct ChannelStreamEnded {
  Token channel;
  bool input;
};

struct ChannelLost {
  Token channel;
  BluetoothError error;
};

struct ServicesChanged {
  Token link;
};

struct LimitsChanged {
  Token link;
  WriteLimits limits;
};

struct ValueChanged {
  Token link;
  Token attribute;
  Bytes value;
};

enum class AttStatus : std::uint8_t {
  Success = 0,
  ReadNotPermitted = 2,
  WriteNotPermitted = 3,
  RequestNotSupported = 6,
  InvalidOffset = 7,
  InvalidValueLength = 13,
  UnlikelyError = 14,
};

struct ServerRequest {
  Token server;
  Token attribute;
  bool write;
  std::size_t offset;
  Bytes value;
  std::optional<std::size_t> read_limit;
  std::function<void(AttStatus, Bytes)> respond;
  std::function<bool()> is_current;
};

struct ServerSubscription {
  Token server;
  Token attribute;
  bool subscribed;
};

struct PeripheralFailure {
  Token session;
  BluetoothError error;
};

using BackendEvent =
    std::variant<AdapterStatus, ScanReport, ScanFailure, LinkLost, ServicesChanged, LimitsChanged, ValueChanged,
                 ServerRequest, ServerSubscription, PeripheralFailure, ChannelStreamEnded, ChannelLost>;

class Backend {
public:
  virtual ~Backend() = default;

  virtual AdapterStatus Status() const = 0;

  virtual BluetoothCapabilities PeripheralCapabilities() const { return {}; }

  virtual BluetoothCapabilities ChannelCapabilities() const;

  virtual Cancellation Invoke(Request request, Completion completion) = 0;

  virtual void StopScan(Token scan) = 0;

  virtual void Disconnect(Token link) = 0;

  virtual void CloseChannel(Token) {}

  virtual void StopAdvertising(Token) {}

  virtual void CloseServer(Token) {}
};

using BackendEvents = std::function<void(BackendEvent)>;

class PeripheralBackend {
public:
  virtual ~PeripheralBackend() = default;

  virtual BluetoothCapabilities Capabilities() const = 0;

  virtual Cancellation Invoke(Request request, Completion completion) = 0;

  virtual void StopAdvertising(Token session) = 0;

  virtual void CloseServer(Token session) = 0;
};

BluetoothError Error(BluetoothErrorCode code, std::string message, std::string domain = {},
                     std::optional<std::int64_t> platform_code = {});

Reply Ok(ReplyValue value = {});

Reply Fail(BluetoothErrorCode code, std::string message);

std::optional<std::string> NormalizeUuid(std::string_view input);

BluetoothCapabilities PlatformCapabilities();

void RegisterPlatform(ApplicationContext& context);

std::unique_ptr<Backend> CreateBackend(TaskScope tasks, BackendEvents events);

} // namespace huxerui::bluetooth::detail
