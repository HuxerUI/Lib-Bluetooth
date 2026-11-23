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
  std::optional<BluetoothError> discovery_error;
  std::size_t pending = 0;
  bool discovering = false;
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
    for (auto& [token, peer] : peers_) {
      peer.peripheral.delegate = nil;
      [central_ cancelPeripheralConnection:peer.peripheral];
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
    auto entry = peers_.extract(link);
    if (entry.empty())
      return;
    auto* peripheral = entry.mapped().peripheral;
    peripheral.delegate = nil;
    if (peripheral.state != CBPeripheralStateDisconnected) {
      retiring_.insert(DeviceId(peripheral));
      [central_ cancelPeripheralConnection:peripheral];
    }
  }

  void ManagerChanged() {
    events_(Status());
    if (Status() == AdapterStatus::PoweredOff || Status() == AdapterStatus::Unavailable)
      retiring_.clear();
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
    if (auto* peer = Find(peripheral); peer && peer->request.operation == Operation::Connect) {
      peripheral.delegate = delegate_;
      Complete(*peer, Ok(WriteLimits{
          [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithResponse],
          [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]}));
    }
  }

  void Disconnected(CBPeripheral* peripheral, NSError* error) {
    if (retiring_.erase(DeviceId(peripheral))) {
      if (auto* peer = Find(peripheral))
        [central_ connectPeripheral:peer->peripheral options:nil];
      return;
    }
    if (auto* peer = Find(peripheral)) {
      auto failure = PlatformError(error, Code::Disconnected, "The Bluetooth device disconnected");
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
    if (request.operation == Operation::Restore || request.operation == Operation::Connect) {
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
      } else {
        auto* peripheral = found->second;
        auto& peer = peers_.emplace(request.link, Peer{.link = request.link, .peripheral = peripheral,
                                                       .request = std::move(request), .result = std::move(reply)})
                         .first->second;
        if (!retiring_.contains(DeviceId(peripheral))) {
          if (peripheral.state == CBPeripheralStateConnected)
            Connected(peripheral);
          else
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
};

BluetoothCapabilities PlatformCapabilities() {
  return {.can_scan = true, .can_restore_device = true, .can_use_gatt_client = true, .can_access_descriptors = true};
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
