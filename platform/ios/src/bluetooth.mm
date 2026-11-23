#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "backend.h"

#include <cstring>
#include <set>
#include <utility>

namespace huxerui::bluetooth::detail {
class IOSBackend;
std::unique_ptr<PeripheralBackend> CreateIOSPeripheral(BackendEvents events, bool configured);
}

@interface HUXIOSBluetoothDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, assign) huxerui::bluetooth::detail::IOSBackend* owner;
@end

namespace huxerui::bluetooth::detail {
namespace {
class StreamChannel;
}
}

@interface HUXIOSChannelStreamDelegate : NSObject <NSStreamDelegate>
@property(nonatomic, assign) huxerui::bluetooth::detail::StreamChannel* owner;
@end

namespace huxerui::bluetooth::detail {

namespace {

using Code = BluetoothErrorCode;

std::string Text(NSString* text) {
  return text ? std::string(text.UTF8String) : std::string{};
}

NSString* Text(const std::string& text) {
  return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

std::string DeviceId(CBPeripheral* peripheral) {
  return Text(peripheral.identifier.UUIDString);
}

FoundDevice Describe(CBPeripheral* peripheral) {
  return {DeviceId(peripheral), peripheral.name ? std::optional(Text(peripheral.name)) : std::nullopt};
}

Bytes CopyBytes(NSData* data) {
  Bytes bytes(data.length);
  if (!bytes.empty())
    std::memcpy(bytes.data(), data.bytes, bytes.size());
  return bytes;
}

NSData* Data(const Bytes& value) {
  return [NSData dataWithBytes:value.data() length:value.size()];
}

BluetoothError PlatformError(NSError* error, Code fallback, std::string message) {
  if (!error)
    return Error(fallback, std::move(message));
  auto code = fallback;
  if ([error.domain isEqualToString:CBATTErrorDomain]) {
    switch (static_cast<CBATTError>(error.code)) {
      case CBATTErrorInsufficientAuthentication:
      case CBATTErrorInsufficientEncryption:
      case CBATTErrorInsufficientEncryptionKeySize:
      case CBATTErrorInsufficientAuthorization:
        code = Code::AuthenticationRequired;
        break;
      case CBATTErrorReadNotPermitted:
      case CBATTErrorWriteNotPermitted:
        code = Code::UnsupportedProperty;
        break;
      case CBATTErrorInvalidAttributeValueLength:
        code = Code::PayloadTooLarge;
        break;
      default: break;
    }
  } else if ([error.domain isEqualToString:CBErrorDomain]) {
    if (error.code == CBErrorConnectionTimeout)
      code = Code::TimedOut;
    else if (error.code == CBErrorPeerRemovedPairingInformation)
      code = Code::AuthenticationFailed;
    else if (error.code == CBErrorOperationCancelled)
      code = Code::Canceled;
  }
  return Error(code, Text(error.localizedDescription), Text(error.domain), error.code);
}

AdapterStatus StatusOf(CBCentralManager* manager) {
  if (!manager)
    return AdapterStatus::Unknown;
  switch (manager.state) {
    case CBManagerStatePoweredOn: return AdapterStatus::PoweredOn;
    case CBManagerStatePoweredOff: return AdapterStatus::PoweredOff;
    case CBManagerStateUnsupported: return AdapterStatus::Unavailable;
    default: return AdapterStatus::Unknown;
  }
}

struct Peer {
  Token link;
  CBPeripheral* peripheral;
  Request request{.operation = Operation::Connect};
  Completion result;
  std::map<Token, CBCharacteristic*> characteristics;
  std::map<Token, CBDescriptor*> descriptors;
  std::set<CBCharacteristic*> stopping_notifications;
  std::set<CBCharacteristic*> unavailable_descriptors;
  std::set<CBCharacteristic*> retirement_notifications;
  std::optional<BluetoothError> discovery_error;
  std::size_t pending = 0;
  bool discovering = false;
  bool retired = false;
};

GattCharacteristicProperties Properties(CBCharacteristic* characteristic) {
  const auto properties = characteristic.properties;
  return {
      .can_read = (properties & CBCharacteristicPropertyRead) != 0,
      .can_write_with_response = (properties & CBCharacteristicPropertyWrite) != 0,
      .can_write_without_response = (properties & CBCharacteristicPropertyWriteWithoutResponse) != 0,
      .can_notify = (properties & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyNotifyEncryptionRequired)) != 0,
      .can_indicate =
          (properties & (CBCharacteristicPropertyIndicate | CBCharacteristicPropertyIndicateEncryptionRequired)) != 0,
  };
}

class StreamChannel final : public std::enable_shared_from_this<StreamChannel> {
public:
  StreamChannel(CBL2CAPChannel* channel, Token id, BackendEvents events)
      : channel_(channel), delegate_([HUXIOSChannelStreamDelegate new]), id_(id), events_(std::move(events)) {}

  ~StreamChannel() {
    delegate_.owner = nullptr;
    for (NSStream* stream in @[channel_.inputStream, channel_.outputStream]) {
      stream.delegate = nil;
      [stream removeFromRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      [stream close];
    }
  }

  void Start() {
    delegate_.owner = this;
    for (NSStream* stream in @[channel_.inputStream, channel_.outputStream]) {
      stream.delegate = delegate_;
      [stream scheduleInRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      [stream open];
    }
  }

  void Read(Completion reply) {
    reader_ = std::move(reply);
    ReadAvailable();
  }

  void Write(Bytes value, Completion reply) {
    writer_ = std::move(reply);
    value_ = std::move(value);
    offset_ = 0;
    WriteAvailable();
  }

  void Event(NSStream* stream, NSStreamEvent event) {
    auto owner = shared_from_this();
    switch (event) {
      case NSStreamEventHasBytesAvailable:
        ReadAvailable();
        break;
      case NSStreamEventHasSpaceAvailable:
        WriteAvailable();
        break;
      case NSStreamEventEndEncountered:
        // The two stream delegates can report EOF independently.
        ReportEnd(stream == channel_.inputStream);
        break;
      case NSStreamEventErrorOccurred:
        events_(ChannelLost{id_, PlatformError(stream.streamError, Code::Disconnected,
                                               "The L2CAP stream closed unexpectedly")});
        break;
      default:
        break;
    }
  }

private:
  void ReportEnd(bool input) {
    auto& ended = input ? input_ended_ : output_ended_;
    if (!std::exchange(ended, true))
      events_(ChannelStreamEnded{.channel = id_, .input = input});
  }

  void ReadAvailable() {
    if (!reader_ || input_ended_)
      return;
    auto* input = channel_.inputStream;
    if (input.hasBytesAvailable) {
      Bytes value(16384);
      const auto count = [input read:reinterpret_cast<std::uint8_t*>(value.data()) maxLength:value.size()];
      if (count < 0) {
        std::exchange(reader_, {})(Reply::Failure(PlatformError(input.streamError, Code::Disconnected, {})));
        return;
      }
      if (count == 0) {
        ReportEnd(true);
        return;
      }
      value.resize(static_cast<std::size_t>(count));
      std::exchange(reader_, {})(Ok(std::move(value)));
    } else if (input.streamStatus == NSStreamStatusAtEnd) {
      ReportEnd(true);
    }
  }

  void WriteAvailable() {
    if (!writer_ || output_ended_)
      return;
    auto* output = channel_.outputStream;
    if (output.streamStatus == NSStreamStatusAtEnd) {
      ReportEnd(false);
      return;
    }
    if (output.streamStatus == NSStreamStatusClosed) {
      std::exchange(writer_, {})(Fail(Code::Disconnected, "The L2CAP output stream is closed"));
      return;
    }
    while (offset_ < value_.size() && output.hasSpaceAvailable) {
      const auto count = [output write:reinterpret_cast<const std::uint8_t*>(value_.data() + offset_)
                             maxLength:value_.size() - offset_];
      if (count < 0) {
        std::exchange(writer_, {})(Reply::Failure(PlatformError(output.streamError, Code::Disconnected, {})));
        return;
      }
      if (count == 0)
        return;
      offset_ += static_cast<std::size_t>(count);
    }
    if (offset_ == value_.size()) {
      value_ = {};
      std::exchange(writer_, {})(Ok());
    }
  }

  CBL2CAPChannel* channel_;
  HUXIOSChannelStreamDelegate* delegate_;
  Token id_;
  BackendEvents events_;
  Completion reader_;
  Completion writer_;
  Bytes value_;
  std::size_t offset_ = 0;
  bool input_ended_ = false;
  bool output_ended_ = false;
};

struct L2capOpen {
  Token id;
  CBL2CAPPSM psm;
  CBPeripheral* peripheral;
  Completion reply;
  bool dispatched = false;
};

struct L2capChannel {
  CBPeripheral* peripheral;
  std::shared_ptr<StreamChannel> streams;
};

} // namespace

class IOSBackend final : public Backend {
public:
  explicit IOSBackend(BackendEvents events) : events_(std::move(events)), delegate_([HUXIOSBluetoothDelegate new]) {
    peripheral_ = CreateIOSPeripheral(events_, configured_);
    delegate_.owner = this;
    if (CBManager.authorization == CBManagerAuthorizationAllowedAlways)
      OpenManager();
  }

  ~IOSBackend() override {
    delegate_.owner = nullptr;
    central_.delegate = nil;
    if (scan_)
      [central_ stopScan];
    channels_.clear();
    for (auto& [id, peripheral] : devices_) {
      if (peripheral.delegate == delegate_)
        peripheral.delegate = nil;
      if (peripheral.state != CBPeripheralStateDisconnected)
        [central_ cancelPeripheralConnection:peripheral];
    }
  }

  AdapterStatus Status() const override {
    return configured_ ? StatusOf(central_) : AdapterStatus::Unavailable;
  }

  BluetoothCapabilities PeripheralCapabilities() const override {
    if (Status() == AdapterStatus::Unavailable)
      return {};
    return peripheral_->Capabilities();
  }

  void StopAdvertising(Token id) override {
    peripheral_->StopAdvertising(id);
  }

  void CloseServer(Token id) override {
    peripheral_->CloseServer(id);
  }

  Cancellation Invoke(Request request, Completion reply) override {
    if (request.operation == Operation::ChannelRead || request.operation == Operation::ChannelWrite) {
      auto found = channels_.find(request.link);
      if (found == channels_.end())
        reply(Fail(Code::Disconnected, "The L2CAP channel is closed"));
      else if (request.operation == Operation::ChannelRead)
        found->second.streams->Read(std::move(reply));
      else
        found->second.streams->Write(std::move(request.value), std::move(reply));
      return {};
    }
    if (IsPeripheralOperation(request.operation))
      return peripheral_->Invoke(std::move(request), std::move(reply));
    if (CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
      reply(Fail(Code::PermissionDenied, "Grant Bluetooth access before using Core Bluetooth"));
      return {};
    }
    OpenManager();
    if (central_.state == CBManagerStateUnknown || central_.state == CBManagerStateResetting) {
      const auto id = next_++;
      pending_.emplace(id, std::pair{std::move(request), std::move(reply)});
      return [this, id] { pending_.erase(id); };
    }
    InvokeReady(std::move(request), std::move(reply));
    return {};
  }

  void StopScan(Token scan) override {
    std::erase_if(pending_, [scan](const auto& entry) { return entry.second.first.scan == scan; });
    if (scan_ == scan) {
      scan_ = 0;
      [central_ stopScan];
    }
  }

  void Disconnect(Token link) override {
    std::erase_if(pending_, [link](const auto& entry) { return entry.second.first.link == link; });
    auto found = peers_.find(link);
    if (found == peers_.end())
      return;
    auto* peripheral = found->second.peripheral;
    auto& peer = found->second;
    if (UsesL2cap(peripheral)) {
      peer.retired = true;
      for (auto& [id, characteristic] : peer.characteristics) {
        if (characteristic.isNotifying ||
            (peer.result && peer.request.operation == Operation::Notify && peer.request.attribute == id)) {
          peer.retirement_notifications.insert(characteristic);
          [peripheral setNotifyValue:NO forCharacteristic:characteristic];
        }
      }
      if ((peer.result && peer.request.operation != Operation::Connect) || !peer.retirement_notifications.empty())
        return;
    }
    peers_.erase(found);
    MaybeDisconnect(peripheral);
  }

  void CloseChannel(Token id) override {
    std::erase_if(pending_, [id](const auto& entry) { return entry.second.first.link == id; });
    if (auto channel = channels_.extract(id); !channel.empty()) {
      auto* peripheral = channel.mapped().peripheral;
      channel.mapped().streams.reset();
      MaybeDisconnect(peripheral);
      return;
    }
    for (auto open = opening_.begin(); open != opening_.end(); ++open) {
      if (open->second.id == id) {
        auto* peripheral = open->second.peripheral;
        // A dispatched open keeps its slot until the delegate callback or link teardown.
        if (open->second.dispatched)
          open->second.reply = {};
        else
          opening_.erase(open);
        MaybeDisconnect(peripheral);
        return;
      }
    }
  }

  void L2capOpened(CBPeripheral* peripheral, CBL2CAPChannel* channel, NSError* error) {
    const auto id = DeviceId(peripheral);
    auto open = opening_.find(id);
    if (open == opening_.end() || !open->second.dispatched ||
        (channel && channel.PSM != open->second.psm)) {
      [channel.inputStream close];
      [channel.outputStream close];
      return;
    }
    auto attempt = opening_.extract(open);
    const auto token = attempt.mapped().id;
    auto reply = std::move(attempt.mapped().reply);
    if (!reply || error || !channel) {
      [channel.inputStream close];
      [channel.outputStream close];
      if (reply)
        reply(Reply::Failure(PlatformError(error, Code::DeviceUnavailable, "The LE L2CAP endpoint is unavailable")));
      MaybeDisconnect(peripheral);
      return;
    }
    auto streams = std::make_shared<StreamChannel>(channel, token, events_);
    channels_.emplace(token, L2capChannel{peripheral, streams});
    streams->Start();
    reply(Ok());
  }

  void ManagerChanged() {
    events_(Status());
    if (Status() == AdapterStatus::PoweredOff || Status() == AdapterStatus::Unavailable) {
      retiring_.clear();
      std::erase_if(peers_, [](const auto& entry) { return entry.second.retired; });
      auto opening = std::exchange(opening_, {});
      for (auto& [id, attempt] : opening) {
        if (attempt.reply)
          attempt.reply(Fail(Status() == AdapterStatus::PoweredOff ? Code::AdapterOff : Code::Unavailable,
                             "The Core Bluetooth adapter is unavailable"));
      }
    }
    if (central_.state == CBManagerStateUnknown || central_.state == CBManagerStateResetting)
      return;
    auto pending = std::exchange(pending_, {});
    for (auto& [id, pair] : pending)
      InvokeReady(std::move(pair.first), std::move(pair.second));
  }

  void Discovered(CBPeripheral* peripheral, NSDictionary<NSString*, id>* values, NSNumber* signal) {
    if (!scan_)
      return;
    auto device = Describe(peripheral);
    devices_.insert_or_assign(device.id, peripheral);
    Advertisement advertisement;
    if (NSString* name = values[CBAdvertisementDataLocalNameKey])
      advertisement.local_name = Text(name);
    for (NSString* key in @[CBAdvertisementDataServiceUUIDsKey, CBAdvertisementDataOverflowServiceUUIDsKey])
      for (CBUUID* uuid in values[key])
        advertisement.service_uuids.push_back(Text(uuid.UUIDString));
    if (NSDictionary<CBUUID*, NSData*>* service_data = values[CBAdvertisementDataServiceDataKey]) {
      for (CBUUID* uuid in service_data) {
        const auto text = Text(uuid.UUIDString);
        advertisement.service_data.emplace(NormalizeUuid(text).value_or(text), CopyBytes(service_data[uuid]));
      }
    }
    if (NSData* manufacturer = values[CBAdvertisementDataManufacturerDataKey]; manufacturer.length >= 2) {
      const auto* raw = static_cast<const std::uint8_t*>(manufacturer.bytes);
      const auto company = static_cast<std::uint16_t>(raw[0] | (raw[1] << 8u));
      NSData* payload = [manufacturer subdataWithRange:NSMakeRange(2, manufacturer.length - 2)];
      advertisement.manufacturer_data.emplace(company, CopyBytes(payload));
    }
    if (signal && signal.intValue != 127)
      advertisement.rssi_dbm = signal.intValue;
    if (NSNumber* connectable = values[CBAdvertisementDataIsConnectable])
      advertisement.connectable = connectable.boolValue;
    events_(ScanReport{scan_, std::move(device), std::move(advertisement)});
  }

  void Connected(CBPeripheral* peripheral) {
    if (retiring_.contains(DeviceId(peripheral)))
      return;
    peripheral.delegate = delegate_;
    auto open = opening_.find(DeviceId(peripheral));
    if (open != opening_.end() && open->second.reply && !open->second.dispatched) {
      open->second.dispatched = true;
      [peripheral openL2CAPChannel:open->second.psm];
    }
    if (auto* peer = Find(peripheral); peer && peer->request.operation == Operation::Connect) {
      Complete(*peer, Ok(WriteLimits{
          [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithResponse],
          [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]}));
    }
  }

  void Disconnected(CBPeripheral* peripheral, NSError* error) {
    const auto id = DeviceId(peripheral);
    auto failure = PlatformError(error, Code::Disconnected, "The Bluetooth device disconnected");
    if (auto open = opening_.extract(id); !open.empty() && open.mapped().reply)
      open.mapped().reply(Reply::Failure(failure));
    for (const auto& [token, channel] : channels_) {
      if (channel.peripheral == peripheral)
        events_(ChannelLost{token, failure});
    }
    if (retiring_.erase(id)) {
      if (auto* peer = Find(peripheral))
        [central_ connectPeripheral:peer->peripheral options:nil];
      return;
    }
    if (auto* peer = Find(peripheral)) {
      if (peer->retired) {
        peers_.erase(peer->link);
        return;
      }
      if (peer->result && peer->request.operation == Operation::Connect)
        Complete(*peer, Reply::Failure(failure));
      events_(LinkLost{peer->link, std::move(failure)});
    }
  }

  void Services(CBPeripheral* peripheral, NSError* error) {
    auto* peer = Find(peripheral);
    if (!peer || !peer->discovering || DiscoveryFailed(*peer, error))
      return;
    peer->pending = peripheral.services.count;
    if (!peer->pending) {
      FinishDiscovery(*peer);
      return;
    }
    for (CBService* service in peripheral.services)
      [peripheral discoverCharacteristics:nil forService:service];
  }

  void Characteristics(CBPeripheral* peripheral, CBService* service, NSError* error) {
    auto* peer = Find(peripheral);
    if (!peer || !peer->discovering)
      return;
    if (error) {
      peer->discovery_error = PlatformError(error, Code::PlatformFailure, {});
    } else {
      peer->pending += service.characteristics.count;
      for (CBCharacteristic* characteristic in service.characteristics)
        [peripheral discoverDescriptorsForCharacteristic:characteristic];
    }
    DiscoveryStep(*peer);
  }

  void Descriptors(CBPeripheral* peripheral, CBCharacteristic* characteristic, NSError* error) {
    if (auto* peer = Find(peripheral); peer && peer->discovering) {
      if (error)
        peer->unavailable_descriptors.insert(characteristic);
      DiscoveryStep(*peer);
    }
  }

  void Changed(CBPeripheral* peripheral) {
    if (auto* peer = Find(peripheral)) {
      if (peer->discovering) {
        peer->discovering = false;
        auto failure = Error(Code::StaleAttribute, "The GATT database changed during discovery");
        Complete(*peer, Reply::Failure(failure));
        events_(LinkLost{peer->link, std::move(failure)});
        return;
      }
      events_(ServicesChanged{peer->link});
    }
  }

  void Value(CBPeripheral* peripheral, CBCharacteristic* characteristic, NSError* error) {
    if (auto* peer = Find(peripheral)) {
      for (const auto& [id, value] : peer->characteristics) {
        if (value != characteristic)
          continue;
        if (Matches(*peer, Operation::Read, id, false)) {
          Complete(*peer, error ? Reply::Failure(PlatformError(error, Code::PlatformFailure, {}))
                                : Ok(CopyBytes(characteristic.value)));
        } else if (!error && characteristic.isNotifying) {
          events_(ValueChanged{peer->link, id, CopyBytes(characteristic.value)});
        }
        return;
      }
    }
  }

  void Value(CBPeripheral* peripheral, CBDescriptor* descriptor, NSError* error) {
    auto* peer = Find(peripheral);
    if (!peer)
      return;
    const auto found = peer->descriptors.find(peer->request.attribute);
    if (found == peer->descriptors.end() || found->second != descriptor ||
        !Matches(*peer, Operation::Read, found->first, true))
      return;
    if (error) {
      Complete(*peer, Reply::Failure(PlatformError(error, Code::PlatformFailure, {})));
      return;
    }
    id value = descriptor.value;
    if ([value isKindOfClass:NSData.class]) {
      Complete(*peer, Ok(CopyBytes(value)));
    } else if ([value isKindOfClass:NSString.class]) {
      Complete(*peer, Ok(CopyBytes([value dataUsingEncoding:NSUTF8StringEncoding])));
    } else if ([value isKindOfClass:NSNumber.class]) {
      const auto number = [value unsignedShortValue];
      Complete(*peer, Ok(Bytes{std::byte(number & 0xffu), std::byte(number >> 8u)}));
    } else {
      Complete(*peer, Fail(Code::UnsupportedProperty, "Core Bluetooth did not expose descriptor bytes"));
    }
  }

  void Wrote(CBPeripheral* peripheral, CBCharacteristic* characteristic, NSError* error) {
    if (auto* peer = Find(peripheral); peer && peer->request.write_mode == GattWriteMode::WithResponse)
      CompleteCharacteristic(*peer, characteristic, Operation::Write, error);
  }

  void Wrote(CBPeripheral* peripheral, CBDescriptor* descriptor, NSError* error) {
    if (auto* peer = Find(peripheral)) {
      const auto found = peer->descriptors.find(peer->request.attribute);
      if (found != peer->descriptors.end() && found->second == descriptor &&
          Matches(*peer, Operation::Write, found->first, true))
        Complete(*peer, error ? Reply::Failure(PlatformError(error, Code::PlatformFailure, {})) : Ok());
    }
  }

  void NotifyState(CBPeripheral* peripheral, CBCharacteristic* characteristic, NSError* error) {
    if (auto* peer = Find(peripheral)) {
      if (peer->retired && (error || !characteristic.isNotifying))
        peer->retirement_notifications.erase(characteristic);
      if (peer->discovering && peer->stopping_notifications.erase(characteristic)) {
        if (error)
          peer->discovery_error = PlatformError(error, Code::PlatformFailure, {});
        if (peer->stopping_notifications.empty()) {
          if (peer->discovery_error)
            FinishDiscovery(*peer);
          else
            [peripheral discoverServices:nil];
        }
        return;
      }
      CompleteCharacteristic(*peer, characteristic, Operation::Notify, error);
    }
  }

  void WriteReady(CBPeripheral* peripheral) {
    if (auto* peer = Find(peripheral); peer && peer->result && peer->request.operation == Operation::Write &&
        peer->request.write_mode == GattWriteMode::WithoutResponse) {
      if (const auto found = peer->characteristics.find(peer->request.attribute);
          found != peer->characteristics.end())
        WriteWithoutResponse(*peer, found->second);
    }
  }

private:
  void OpenManager() {
    if (!central_ && configured_)
      central_ = [[CBCentralManager alloc] initWithDelegate:delegate_ queue:dispatch_get_main_queue()
                                                  options:@{CBCentralManagerOptionShowPowerAlertKey: @NO}];
  }

  Peer* Find(CBPeripheral* peripheral) {
    for (auto& [id, peer] : peers_) {
      if (peer.peripheral == peripheral)
        return &peer;
    }
    return nullptr;
  }

  bool UsesL2cap(CBPeripheral* peripheral) const {
    const auto open = opening_.find(DeviceId(peripheral));
    if (open != opening_.end() && open->second.reply)
      return true;
    return std::ranges::any_of(channels_, [peripheral](const auto& entry) {
      return entry.second.peripheral == peripheral;
    });
  }

  void MaybeDisconnect(CBPeripheral* peripheral) {
    auto* peer = Find(peripheral);
    if ((peer && !peer->retired) || UsesL2cap(peripheral))
      return;
    if (peer)
      peers_.erase(peer->link);
    peripheral.delegate = nil;
    if (central_.state == CBManagerStatePoweredOn && peripheral.state != CBPeripheralStateDisconnected &&
        retiring_.insert(DeviceId(peripheral)).second)
      [central_ cancelPeripheralConnection:peripheral];
  }

  void OpenL2cap(const Request& request, CBPeripheral* peripheral, Completion reply) {
    const auto id = DeviceId(peripheral);
    if (opening_.contains(id) || retiring_.contains(id)) {
      reply(Fail(Code::Busy, "A Core Bluetooth channel or link is still opening or closing"));
      return;
    }
    opening_.emplace(id, L2capOpen{request.link, request.psm, peripheral, std::move(reply)});
    if (peripheral.state == CBPeripheralStateConnected)
      Connected(peripheral);
    else if (peripheral.state != CBPeripheralStateConnecting)
      [central_ connectPeripheral:peripheral options:nil];
  }

  void InvokeReady(Request request, Completion reply) {
    if (central_.state != CBManagerStatePoweredOn && request.operation != Operation::Restore) {
      reply(Fail(central_.state == CBManagerStatePoweredOff ? Code::AdapterOff : Code::Unavailable,
                 "The Core Bluetooth adapter is unavailable"));
      return;
    }
    if (request.operation == Operation::Scan) {
      scan_ = request.scan;
      NSMutableArray<CBUUID*>* services = [NSMutableArray array];
      for (const auto& uuid : request.advertised_services)
        [services addObject:[CBUUID UUIDWithString:Text(uuid)]];
      [central_ scanForPeripheralsWithServices:services.count ? services : nil
                                      options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
      reply(Ok());
      return;
    }
    if (request.operation == Operation::Restore || request.operation == Operation::Connect ||
        request.operation == Operation::OpenL2cap) {
      auto found = devices_.find(request.device);
      if (found == devices_.end()) {
        auto* uuid = [[NSUUID alloc] initWithUUIDString:Text(request.device)];
        auto* peripheral = uuid ? [central_ retrievePeripheralsWithIdentifiers:@[uuid]].firstObject : nil;
        if (peripheral)
          found = devices_.emplace(request.device, peripheral).first;
      }
      if (found == devices_.end()) {
        reply(Fail(Code::DeviceUnavailable, "Core Bluetooth no longer recognizes this device"));
        return;
      }
      if (request.operation == Operation::Restore) {
        reply(Ok(Describe(found->second)));
      } else if (request.operation == Operation::OpenL2cap) {
        OpenL2cap(request, found->second, std::move(reply));
      } else {
        auto* peripheral = found->second;
        if (auto* retired = Find(peripheral); retired && retired->retired) {
          if (retired->result || !retired->retirement_notifications.empty()) {
            reply(Fail(Code::Busy, "The previous GATT request is still retiring"));
            return;
          }
          peers_.erase(retired->link);
        }
        auto& peer = peers_.emplace(request.link, Peer{.link = request.link, .peripheral = peripheral,
                                                       .request = std::move(request), .result = std::move(reply)})
                         .first->second;
        if (!retiring_.contains(DeviceId(peripheral))) {
          if (peripheral.state == CBPeripheralStateConnected)
            Connected(peripheral);
          else if (peripheral.state != CBPeripheralStateConnecting)
            [central_ connectPeripheral:peer.peripheral options:nil];
        }
      }
      return;
    }
    auto found = peers_.find(request.link);
    if (found == peers_.end()) {
      reply(Fail(Code::Disconnected, "The Core Bluetooth connection is closed"));
      return;
    }
    auto& peer = found->second;
    peer.request = std::move(request);
    peer.result = std::move(reply);
    if (peer.request.operation == Operation::Discover) {
      peer.discovering = true;
      peer.pending = 0;
      peer.discovery_error.reset();
      peer.unavailable_descriptors.clear();
      for (auto& [id, characteristic] : peer.characteristics) {
        if (characteristic.isNotifying) {
          peer.stopping_notifications.insert(characteristic);
          [peer.peripheral setNotifyValue:NO forCharacteristic:characteristic];
        }
      }
      if (peer.stopping_notifications.empty())
        [peer.peripheral discoverServices:nil];
      return;
    }
    InvokeAttribute(peer);
  }

  void InvokeAttribute(Peer& peer) {
    auto& request = peer.request;
    if (request.descriptor) {
      const auto found = peer.descriptors.find(request.attribute);
      if (found == peer.descriptors.end()) {
        Complete(peer, Fail(Code::StaleAttribute, "The descriptor is no longer available"));
      } else if (request.operation == Operation::Read) {
        [peer.peripheral readValueForDescriptor:found->second];
      } else {
        [peer.peripheral writeValue:Data(request.value) forDescriptor:found->second];
      }
      return;
    }
    const auto found = peer.characteristics.find(request.attribute);
    if (found == peer.characteristics.end()) {
      Complete(peer, Fail(Code::StaleAttribute, "The characteristic is no longer available"));
      return;
    }
    auto* characteristic = found->second;
    switch (request.operation) {
      case Operation::Read:
        if (characteristic.isNotifying)
          Complete(peer, Fail(Code::Busy, "Stop notifications before reading this characteristic on Core Bluetooth"));
        else
          [peer.peripheral readValueForCharacteristic:characteristic];
        break;
      case Operation::Write:
        if (request.write_mode == GattWriteMode::WithResponse) {
          [peer.peripheral writeValue:Data(request.value)
                    forCharacteristic:characteristic
                                 type:CBCharacteristicWriteWithResponse];
        } else {
          WriteWithoutResponse(peer, characteristic);
        }
        break;
      case Operation::Notify:
        [peer.peripheral setNotifyValue:request.enabled forCharacteristic:characteristic];
        break;
      default:
        Complete(peer, Fail(Code::Unsupported, "Unsupported Core Bluetooth operation"));
        break;
    }
  }

  void WriteWithoutResponse(Peer& peer, CBCharacteristic* characteristic) {
    if (peer.peripheral.canSendWriteWithoutResponse) {
      [peer.peripheral writeValue:Data(peer.request.value)
                forCharacteristic:characteristic
                             type:CBCharacteristicWriteWithoutResponse];
      Complete(peer, Ok());
    }
  }

  bool DiscoveryFailed(Peer& peer, NSError* error) {
    if (!error)
      return false;
    peer.discovering = false;
    Complete(peer, Reply::Failure(PlatformError(error, Code::PlatformFailure, {})));
    return true;
  }

  void DiscoveryStep(Peer& peer) {
    if (peer.pending && --peer.pending == 0)
      FinishDiscovery(peer);
  }

  void FinishDiscovery(Peer& peer) {
    peer.discovering = false;
    if (peer.discovery_error) {
      Complete(peer, Reply::Failure(std::move(*peer.discovery_error)));
      events_(ServicesChanged{peer.link});
      return;
    }
    peer.characteristics.clear();
    peer.descriptors.clear();
    Discovery discovery;
    for (CBService* service in peer.peripheral.services) {
      RemoteService output{Text(service.UUID.UUIDString), {}};
      for (CBCharacteristic* characteristic in service.characteristics) {
        const auto id = next_++;
        peer.characteristics.emplace(id, characteristic);
        RemoteCharacteristic attr{id, Text(characteristic.UUID.UUIDString), Properties(characteristic), {}};
        if (characteristic.descriptors && !peer.unavailable_descriptors.contains(characteristic)) {
          attr.descriptors.emplace();
          for (CBDescriptor* descriptor in characteristic.descriptors) {
            const auto descriptor_id = next_++;
            peer.descriptors.emplace(descriptor_id, descriptor);
            attr.descriptors->push_back({descriptor_id, Text(descriptor.UUID.UUIDString)});
          }
        }
        output.characteristics.push_back(std::move(attr));
      }
      discovery.services.push_back(std::move(output));
    }
    Complete(peer, Ok(std::move(discovery)));
  }

  static bool Matches(const Peer& peer, Operation operation, Token attribute, bool descriptor) {
    return peer.result && peer.request.operation == operation && peer.request.attribute == attribute &&
           peer.request.descriptor == descriptor;
  }

  void CompleteCharacteristic(Peer& peer, CBCharacteristic* characteristic, Operation operation, NSError* error) {
    const auto found = peer.characteristics.find(peer.request.attribute);
    if (found != peer.characteristics.end() && found->second == characteristic &&
        Matches(peer, operation, found->first, false))
      Complete(peer, error ? Reply::Failure(PlatformError(error, Code::PlatformFailure, {})) : Ok());
  }

  static void Complete(Peer& peer, Reply result) {
    if (auto reply = std::exchange(peer.result, {}))
      reply(std::move(result));
  }

  BackendEvents events_;
  std::unique_ptr<PeripheralBackend> peripheral_;
  HUXIOSBluetoothDelegate* delegate_;
  CBCentralManager* central_ = nil;
  const bool configured_ = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"] != nil;
  Token scan_ = 0;
  Token next_ = 1;
  std::map<Token, std::pair<Request, Completion>> pending_;
  std::map<std::string, CBPeripheral*> devices_;
  std::map<Token, Peer> peers_;
  std::set<std::string> retiring_;
  std::map<std::string, L2capOpen> opening_;
  std::map<Token, L2capChannel> channels_;
};

BluetoothCapabilities PlatformCapabilities() {
  return {.can_scan = true, .can_restore_device = true, .can_use_gatt_client = true, .can_access_descriptors = true,
          .can_connect_le_l2cap = true};
}

void RegisterPlatform(ApplicationContext&) {}

std::unique_ptr<Backend> CreateBackend(TaskScope, BackendEvents events) {
  return std::make_unique<IOSBackend>(std::move(events));
}

} // namespace huxerui::bluetooth::detail

@implementation HUXIOSBluetoothDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  if (_owner)
    _owner->ManagerChanged();
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
         advertisementData:(NSDictionary<NSString*, id>*)values
                       RSSI:(NSNumber*)signal {
  if (_owner)
    _owner->Discovered(peripheral, values, signal);
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
  if (_owner)
    _owner->Connected(peripheral);
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
  if (_owner)
    _owner->Disconnected(peripheral, error);
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                       error:(NSError*)error {
  if (_owner)
    _owner->Disconnected(peripheral, error);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didOpenL2CAPChannel:(CBL2CAPChannel*)channel
                  error:(NSError*)error {
  if (_owner)
    _owner->L2capOpened(peripheral, channel, error);
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
  if (_owner)
    _owner->Services(peripheral, error);
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error {
  if (_owner)
    _owner->Characteristics(peripheral, service, error);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverDescriptorsForCharacteristic:(CBCharacteristic*)characteristic
                                       error:(NSError*)error {
  if (_owner)
    _owner->Descriptors(peripheral, characteristic, error);
}

- (void)peripheral:(CBPeripheral*)peripheral didModifyServices:(NSArray<CBService*>*)services {
  if (_owner)
    _owner->Changed(peripheral);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
                              error:(NSError*)error {
  if (_owner)
    _owner->Value(peripheral, characteristic, error);
}

- (void)peripheral:(CBPeripheral*)peripheral didUpdateValueForDescriptor:(CBDescriptor*)descriptor error:(NSError*)error {
  if (_owner)
    _owner->Value(peripheral, descriptor, error);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
                             error:(NSError*)error {
  if (_owner)
    _owner->Wrote(peripheral, characteristic, error);
}

- (void)peripheral:(CBPeripheral*)peripheral didWriteValueForDescriptor:(CBDescriptor*)descriptor error:(NSError*)error {
  if (_owner)
    _owner->Wrote(peripheral, descriptor, error);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
                                          error:(NSError*)error {
  if (_owner)
    _owner->NotifyState(peripheral, characteristic, error);
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)peripheral {
  if (_owner)
    _owner->WriteReady(peripheral);
}

@end

@implementation HUXIOSChannelStreamDelegate

- (void)stream:(NSStream*)stream handleEvent:(NSStreamEvent)event {
  if (_owner)
    _owner->Event(stream, event);
}

@end
