#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include "backend.h"

#include <array>
#include <atomic>
#include <utility>

namespace huxerui::bluetooth::detail {
class MacRfcommChannel;
}

@interface HUXMacOSRfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate> {
@public
  std::weak_ptr<huxerui::bluetooth::detail::MacRfcommChannel> owner;
}
@end

namespace huxerui::bluetooth::detail {

namespace {

using Code = BluetoothErrorCode;
constexpr std::string_view classic_prefix = "rfcomm:";

std::string ClassicId(IOBluetoothDevice* device) {
  const char* address = device.addressString.UTF8String;
  return address ? std::string(classic_prefix) + address : std::string{};
}

FoundDevice DescribeClassic(IOBluetoothDevice* device) {
  NSString* name = device.name;
  const auto* text = name.UTF8String;
  return {ClassicId(device), text ? std::optional(std::string(text)) : std::nullopt};
}

IOBluetoothDevice* PairedDevice(std::string_view id) {
  for (IOBluetoothDevice* device in IOBluetoothDevice.pairedDevices)
    if (ClassicId(device) == id)
      return device;
  return nil;
}

IOBluetoothSDPUUID* SdpUuid(std::string_view uuid) {
  std::array<unsigned char, 16> bytes{};
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::size_t index = 0;
  for (std::size_t pos = 0; pos < uuid.size();) {
    if (uuid[pos] == '-') {
      ++pos;
      continue;
    }
    bytes[index++] = static_cast<unsigned char>((digit(uuid[pos]) << 4) | digit(uuid[pos + 1]));
    pos += 2;
  }
  return [IOBluetoothSDPUUID uuidWithBytes:bytes.data() length:bytes.size()];
}

Reply RfcommError(Code code, const char* message, IOReturn status) {
  return Reply::Failure(Error(status == kIOReturnNotPermitted ? Code::PermissionDenied : code, message,
                              "IOBluetooth", status));
}

} // namespace

class MacRfcommChannel final : public std::enable_shared_from_this<MacRfcommChannel> {
public:
  MacRfcommChannel(TaskScope tasks, BackendEvents events, Token id, std::string device_id)
      : tasks_(std::move(tasks)), events_(std::move(events)), id_(id), device_id_(std::move(device_id)) {}

  ~MacRfcommChannel() {
    Disconnect();
  }

  void Open(IOBluetoothDevice* device, IOBluetoothSDPUUID* uuid, Completion completion) {
    delegate_ = [HUXMacOSRfcommDelegate new];
    delegate_->owner = weak_from_this();
    query_ = @[uuid];
    opening_ = std::move(completion);
    pending_ = true;
    const auto status = [device performSDPQuery:delegate_ uuids:query_];
    if (status != kIOReturnSuccess)
      CompleteOpen(RfcommError(Code::DeviceUnavailable, "The RFCOMM SDP query could not start", status));
  }

  void ServiceResolved(IOBluetoothDevice* device, IOReturn status) {
    tasks_.Post([weak = weak_from_this(), device, status] {
      if (auto self = weak.lock())
        self->OpenResolved(device, status);
    });
  }

  void Opened(IOBluetoothRFCOMMChannel* channel, IOReturn status) {
    tasks_.Post([weak = weak_from_this(), channel, status] {
      if (auto self = weak.lock(); self && self->opening_ && status == kIOReturnSuccess) {
        self->channel_ = channel;
        self->CompleteOpen(Ok());
      } else {
        [channel setDelegate:nil];
        [channel closeChannel];
        if (self) {
          self->channel_ = nil;
          self->CompleteOpen(RfcommError(Code::DeviceUnavailable, "The RFCOMM channel could not open", status));
        }
      }
    });
  }

  void Data(const void* data, std::size_t size) {
    if (size == 0)
      return;
    if (overflowed_.load())
      return;
    const auto queued = queued_bytes_.fetch_add(size);
    if (size > channel_read_size || queued > channel_read_size - size) {
      queued_bytes_.fetch_sub(size);
      if (!overflowed_.exchange(true))
        Lost(Error(Code::PlatformFailure, "The RFCOMM receive buffer overflowed"));
      return;
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    tasks_.Post([weak = weak_from_this(), value = Bytes(bytes, bytes + size)]() mutable {
      if (auto self = weak.lock()) {
        self->queued_bytes_.fetch_sub(value.size());
        self->Receive(std::move(value));
      }
    });
  }

  void Lost(BluetoothError error) {
    tasks_.Post([weak = weak_from_this(), error = std::move(error)]() mutable {
      if (auto self = weak.lock()) {
        if (self->pending_)
          self->CompleteOpen(Reply::Failure(error));
        else if (self->channel_)
          self->events_(ChannelLost{self->id_, std::move(error)});
      }
    });
  }

  void Sent(IOReturn status) {
    tasks_.Post([weak = weak_from_this(), status] {
      if (auto self = weak.lock(); self && self->writing_) {
        if (status != kIOReturnSuccess) {
          std::exchange(self->writing_, {})(RfcommError(Code::Disconnected, "The RFCOMM write failed", status));
        } else {
          self->offset_ += self->sent_;
          self->WriteNext();
        }
      }
    });
  }

  void Read(Completion completion) {
    if (input_.empty())
      reading_ = std::move(completion);
    else
      completion(Ok(std::exchange(input_, {})));
  }

  void Write(Bytes value, Completion completion) {
    output_ = std::move(value);
    offset_ = 0;
    writing_ = std::move(completion);
    WriteNext();
  }

  void Close() {
    opening_ = {};
    if (!pending_)
      Disconnect();
  }

  bool IsDone() const {
    return !pending_ && !channel_;
  }

  bool IsOpen() const {
    return channel_ && !pending_;
  }

  const std::string& DeviceId() const {
    return device_id_;
  }

private:
  void OpenResolved(IOBluetoothDevice* device, IOReturn status) {
    if (!opening_ || status != kIOReturnSuccess) {
      CompleteOpen(RfcommError(Code::DeviceUnavailable, "The RFCOMM service could not be resolved", status));
      return;
    }
    std::optional<BluetoothRFCOMMChannelID> endpoint;
    for (IOBluetoothSDPServiceRecord* record in device.services) {
      BluetoothRFCOMMChannelID id = 0;
      if ([record matchesUUIDArray:query_] && [record getRFCOMMChannelID:&id] == kIOReturnSuccess && id) {
        if (endpoint && *endpoint != id) {
          CompleteOpen(Fail(Code::Unsupported, "The RFCOMM UUID resolves to multiple channels"));
          return;
        }
        endpoint = id;
      }
    }
    if (!endpoint) {
      CompleteOpen(Fail(Code::DeviceUnavailable, "The RFCOMM UUID is unavailable on the peer"));
      return;
    }
    IOBluetoothRFCOMMChannel* channel = nil;
    status = [device openRFCOMMChannelAsync:&channel withChannelID:*endpoint delegate:delegate_];
    if (status == kIOReturnSuccess)
      channel_ = channel;
    else
      CompleteOpen(RfcommError(Code::DeviceUnavailable, "The RFCOMM open could not start", status));
  }

  void Receive(Bytes value) {
    if (!channel_)
      return;
    if (auto reader = std::exchange(reading_, {})) {
      reader(Ok(std::move(value)));
    } else if (value.size() > channel_read_size - input_.size()) {
      events_(ChannelLost{id_, Error(Code::PlatformFailure, "The RFCOMM receive buffer overflowed")});
      Disconnect();
    } else {
      input_.insert(input_.end(), value.begin(), value.end());
    }
  }

  void CompleteOpen(Reply result) {
    pending_ = false;
    if (!result.Succeeded())
      Disconnect();
    if (auto completion = std::exchange(opening_, {}))
      completion(std::move(result));
  }

  void WriteNext() {
    if (offset_ == output_.size()) {
      output_ = {};
      std::exchange(writing_, {})(Ok());
      return;
    }
    sent_ = std::min<std::size_t>([channel_ getMTU], output_.size() - offset_);
    const auto status = sent_ ? [channel_ writeAsync:output_.data() + offset_ length:sent_ refcon:nullptr]
                              : kIOReturnNotOpen;
    if (status != kIOReturnSuccess)
      std::exchange(writing_, {})(RfcommError(Code::Disconnected, "The RFCOMM write could not start", status));
  }

  void Disconnect() {
    if (auto* channel = std::exchange(channel_, nil)) {
      [channel setDelegate:nil];
      [channel closeChannel];
    }
    reading_ = {};
    writing_ = {};
  }

  TaskScope tasks_;
  BackendEvents events_;
  Token id_;
  std::string device_id_;
  HUXMacOSRfcommDelegate* delegate_ = nil;
  NSArray<IOBluetoothSDPUUID*>* query_ = nil;
  IOBluetoothRFCOMMChannel* channel_ = nil;
  Completion opening_;
  Completion reading_;
  Completion writing_;
  Bytes input_;
  Bytes output_;
  std::size_t offset_ = 0;
  std::size_t sent_ = 0;
  bool pending_ = false;
  std::atomic<std::size_t> queued_bytes_{0};
  std::atomic<bool> overflowed_{false};
};

namespace {

class RfcommBackend final : public Backend {
public:
  RfcommBackend(TaskScope tasks, BackendEvents events) : tasks_(std::move(tasks)), events_(std::move(events)) {}

  AdapterStatus Status() const override { return AdapterStatus::Unknown; }

  void StopScan(Token) override {}

  void Disconnect(Token) override {}

  void CloseChannel(Token id) override {
    if (auto found = channels_.find(id); found != channels_.end()) {
      found->second->Close();
      if (found->second->IsDone())
        channels_.erase(found);
    }
  }

  Cancellation Invoke(Request request, Completion reply) override {
    if (request.operation == Operation::ListRfcomm) {
      std::vector<FoundDevice> candidates;
      for (IOBluetoothDevice* device in IOBluetoothDevice.pairedDevices)
        candidates.push_back(DescribeClassic(device));
      reply(Ok(std::move(candidates)));
    } else if (request.operation == Operation::Restore || request.operation == Operation::OpenRfcomm) {
      Open(std::move(request), std::move(reply));
    } else if (auto found = channels_.find(request.link); found != channels_.end() && found->second->IsOpen()) {
      if (request.operation == Operation::ChannelRead)
        found->second->Read(std::move(reply));
      else
        found->second->Write(std::move(request.value), std::move(reply));
    } else {
      reply(Fail(Code::Disconnected, "The RFCOMM channel is closed"));
    }
    return {};
  }

private:
  void Open(Request request, Completion reply) {
    auto* device = PairedDevice(request.device);
    if (!device) {
      reply(Fail(Code::DeviceUnavailable, "macOS no longer recognizes this paired Classic device"));
      return;
    }
    if (request.operation == Operation::Restore) {
      reply(Ok(DescribeClassic(device)));
      return;
    }
    std::erase_if(channels_, [](const auto& entry) { return entry.second->IsDone(); });
    if (std::ranges::any_of(channels_, [&](const auto& entry) {
          return entry.second->DeviceId() == request.device;
        })) {
      reply(Fail(Code::Busy, "The device already has an RFCOMM channel or pending open"));
      return;
    }
    auto channel = std::make_shared<MacRfcommChannel>(tasks_, events_, request.link, request.device);
    channels_.emplace(request.link, channel);
    channel->Open(device, SdpUuid(request.service_uuid), std::move(reply));
  }

  TaskScope tasks_;
  BackendEvents events_;
  std::map<Token, std::shared_ptr<MacRfcommChannel>> channels_;
};

} // namespace

std::unique_ptr<Backend> CreateMacRfcomm(TaskScope tasks, BackendEvents events) {
  return std::make_unique<RfcommBackend>(std::move(tasks), std::move(events));
}

} // namespace huxerui::bluetooth::detail

@implementation HUXMacOSRfcommDelegate

- (void)sdpQueryComplete:(IOBluetoothDevice*)device status:(IOReturn)status {
  if (auto channel = owner.lock())
    channel->ServiceResolved(device, status);
}

- (void)rfcommChannelOpenComplete:(IOBluetoothRFCOMMChannel*)channel status:(IOReturn)status {
  if (auto state = owner.lock())
    state->Opened(channel, status);
  else {
    [channel setDelegate:nil];
    [channel closeChannel];
  }
}

- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel*)channel data:(void*)bytes length:(size_t)length {
  if (auto state = owner.lock())
    state->Data(bytes, length);
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel*)channel {
  if (auto state = owner.lock())
    state->Lost(huxerui::bluetooth::detail::Error(huxerui::bluetooth::BluetoothErrorCode::Disconnected,
                                                "The macOS RFCOMM channel closed"));
}

- (void)rfcommChannelWriteComplete:(IOBluetoothRFCOMMChannel*)channel refcon:(void*)refcon status:(IOReturn)status {
  if (auto state = owner.lock())
    state->Sent(status);
}

@end
