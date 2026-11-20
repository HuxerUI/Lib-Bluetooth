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

enum class Operation { Scan, Select, Restore, Bond, Pair, Connect, Discover, Read, Write, Notify };

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
};

using ReplyValue = std::variant<std::monostate, FoundDevice, BondState, WriteLimits, Discovery, Bytes>;
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

using BackendEvent =
    std::variant<AdapterStatus, ScanReport, ScanFailure, LinkLost, ServicesChanged, LimitsChanged, ValueChanged>;

class Backend {
public:
  virtual ~Backend() = default;

  virtual AdapterStatus Status() const = 0;

  virtual Cancellation Invoke(Request request, Completion completion) = 0;

  virtual void StopScan(Token scan) = 0;

  virtual void Disconnect(Token link) = 0;
};

using BackendEvents = std::function<void(BackendEvent)>;

BluetoothError Error(BluetoothErrorCode code, std::string message, std::string domain = {},
                     std::optional<std::int64_t> platform_code = {});

Reply Ok(ReplyValue value = {});

Reply Fail(BluetoothErrorCode code, std::string message);

std::optional<std::string> NormalizeUuid(std::string_view input);

BluetoothCapabilities PlatformCapabilities();

void RegisterPlatform(ApplicationContext& context);

std::unique_ptr<Backend> CreateBackend(TaskScope tasks, BackendEvents events);

} // namespace huxerui::bluetooth::detail
