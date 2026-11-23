#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "backend.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>

namespace huxerui::bluetooth::detail {
class MacOSPeripheralRole;
}

@interface HUXMacOSPeripheralDelegate : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic, assign) huxerui::bluetooth::detail::MacOSPeripheralRole* owner;
@end

namespace huxerui::bluetooth::detail {

namespace {

using Code = BluetoothErrorCode;

NSString* String(const std::string& value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSData* Data(const Bytes& value) {
  return [NSData dataWithBytes:value.data() length:value.size()];
}

Bytes BytesFrom(NSData* data) {
  Bytes value(data.length);
  if (!value.empty())
    std::memcpy(value.data(), data.bytes, value.size());
  return value;
}

BluetoothError PeripheralError(NSError* error, const char* context) {
  const auto code = [error.domain isEqualToString:CBErrorDomain] && error.code == CBErrorOutOfSpace
                        ? Code::PayloadTooLarge : Code::PlatformFailure;
  return Error(code, context, error.domain.UTF8String ?: "", error.code);
}

} // namespace

class MacOSPeripheralRole final {
public:
  MacOSPeripheralRole(Request request, Completion completion, BackendEvents events)
      : request_(std::move(request)), completion_(std::move(completion)), events_(std::move(events)),
        delegate_([HUXMacOSPeripheralDelegate new]) {}

  ~MacOSPeripheralRole() {
    delegate_.owner = nullptr;
    manager_.delegate = nil;
    if (manager_.state == CBManagerStatePoweredOn) {
      if (request_.operation == Operation::Advertise)
        [manager_ stopAdvertising];
      else
        [manager_ removeAllServices];
    }
  }

  void Open() {
    delegate_.owner = this;
    manager_ = [[CBPeripheralManager alloc] initWithDelegate:delegate_ queue:dispatch_get_main_queue()
        options:@{CBPeripheralManagerOptionShowPowerAlertKey: @NO}];
  }

  void StateChanged(CBPeripheralManager* manager) {
    switch (manager.state) {
      case CBManagerStatePoweredOn:
        if (!started_) {
          started_ = true;
          if (request_.operation == Operation::Advertise)
            Advertise(manager);
          else
            RegisterServices(manager);
        }
        return;
      case CBManagerStateUnknown:
        return;
      case CBManagerStateResetting:
        if (!started_)
          return;
        Failure(Error(Code::Unavailable, "The Core Bluetooth peripheral manager reset"));
        return;
      case CBManagerStateUnauthorized:
        Failure(Error(Code::PermissionDenied, "Bluetooth access was denied"));
        return;
      case CBManagerStateUnsupported:
        Failure(Error(Code::Unsupported, "The adapter does not support the peripheral role"));
        return;
      default:
        Failure(Error(Code::AdapterOff, "The Bluetooth adapter is off"));
        return;
    }
  }

  void Added(CBPeripheralManager* manager, CBService* service, NSError* error) {
    if (!completion_ || pending_.empty() || service != pending_.front())
      return;
    if (error) {
      Failure(PeripheralError(error, "Core Bluetooth could not publish the local service"));
      return;
    }
    pending_.pop_front();
    AddNext(manager);
  }

  void AdvertisingStarted(NSError* error) {
    if (!completion_)
      return;
    if (error)
      Failure(PeripheralError(error, "Core Bluetooth could not start advertising"));
    else
      Complete(Ok());
  }

  void Read(CBPeripheralManager* manager, CBATTRequest* request) {
    Dispatch(manager, request, false);
  }

  void Write(CBPeripheralManager* manager, NSArray<CBATTRequest*>* requests) {
    if (requests.count == 1) {
      Dispatch(manager, requests.firstObject, true);
    } else if (requests.firstObject) {
      [manager respondToRequest:requests.firstObject withResult:CBATTErrorRequestNotSupported];
    }
  }

  void Subscription(CBCentral* central, CBCharacteristic* characteristic, bool enabled) {
    const auto found = Find(characteristic);
    if (found == attributes_.end())
      return;
    auto& peers = found->second.peers;
    const bool was_subscribed = !peers.empty();
    auto identifier = std::string(central.identifier.UUIDString.UTF8String);
    if (enabled)
      peers.insert_or_assign(std::move(identifier), central);
    else
      peers.erase(identifier);
    if (was_subscribed != !peers.empty())
      events_(ServerSubscription{request_.peripheral, found->first, !peers.empty()});
  }

  void Notify(Token id, const Bytes& bytes, Completion completion) {
    if (!started_ || completion_ || failed_ || manager_.state != CBManagerStatePoweredOn) {
      completion(Fail(Code::Unavailable, "The local GATT server is unavailable"));
      return;
    }
    const auto found = attributes_.find(id);
    if (found == attributes_.end()) {
      completion(Fail(Code::InvalidArgument, "Unknown local GATT characteristic"));
      return;
    }
    const auto& attribute = found->second;
    if (attribute.peers.empty()) {
      completion(Ok());
      return;
    }
    if (std::ranges::any_of(attribute.peers, [&](const auto& peer) {
          return bytes.size() > peer.second.maximumUpdateValueLength;
        })) {
      completion(Fail(Code::PayloadTooLarge, "The notification exceeds a subscribed peer's update size"));
      return;
    }
    const bool sent = [manager_ updateValue:Data(bytes) forCharacteristic:attribute.characteristic
                      onSubscribedCentrals:nil];
    completion(sent ? Ok() : Fail(Code::Busy, "The Core Bluetooth notification queue is full"));
  }

private:
  struct Attribute {
    CBMutableCharacteristic* characteristic;
    std::map<std::string, CBCentral*> peers;
  };

  std::map<Token, Attribute>::iterator Find(CBCharacteristic* characteristic) {
    return std::ranges::find_if(attributes_, [&](const auto& item) {
      return item.second.characteristic == characteristic;
    });
  }

  void Advertise(CBPeripheralManager* manager) {
    NSMutableDictionary<NSString*, id>* payload = [NSMutableDictionary dictionary];
    NSMutableArray<CBUUID*>* uuids = [NSMutableArray array];
    for (const auto& uuid : request_.advertisement.service_uuids)
      [uuids addObject:[CBUUID UUIDWithString:String(uuid)]];
    if (uuids.count)
      payload[CBAdvertisementDataServiceUUIDsKey] = uuids;
    if (const auto& name = request_.advertisement.local_name) {
      NSString* local_name = String(*name);
      if (!local_name) {
        Failure(Error(Code::InvalidArgument, "The local name must be valid UTF-8"));
        return;
      }
      payload[CBAdvertisementDataLocalNameKey] = local_name;
    }
    [manager startAdvertising:payload];
  }

  void RegisterServices(CBPeripheralManager* manager) {
    for (const auto& service : request_.local_services) {
      CBMutableService* local = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:String(service.uuid)]
                                                            primary:YES];
      NSMutableArray<CBMutableCharacteristic*>* characteristics = [NSMutableArray array];
      for (const auto& definition : service.characteristics) {
        const auto properties = definition.properties;
        CBCharacteristicProperties type = 0;
        CBAttributePermissions permissions = 0;
        if (properties.can_read) {
          type |= CBCharacteristicPropertyRead;
          permissions |= CBAttributePermissionsReadable;
        }
        if (properties.can_write_with_response) {
          type |= CBCharacteristicPropertyWrite;
          permissions |= CBAttributePermissionsWriteable;
        }
        if (properties.can_notify)
          type |= CBCharacteristicPropertyNotify;
        CBMutableCharacteristic* characteristic = [[CBMutableCharacteristic alloc]
            initWithType:[CBUUID UUIDWithString:String(definition.uuid)] properties:type value:nil
             permissions:permissions];
        attributes_.emplace(definition.id, Attribute{characteristic, {}});
        [characteristics addObject:characteristic];
      }
      local.characteristics = characteristics;
      pending_.push_back(local);
    }
    AddNext(manager);
  }

  void AddNext(CBPeripheralManager* manager) {
    if (pending_.empty())
      Complete(Ok());
    else
      [manager addService:pending_.front()];
  }

  void Dispatch(CBPeripheralManager* manager, CBATTRequest* request, bool write) {
    const auto found = Find(request.characteristic);
    if (found == attributes_.end()) {
      [manager respondToRequest:request withResult:CBATTErrorRequestNotSupported];
      return;
    }
    events_(ServerRequest{request_.peripheral, found->first, write, request.offset,
                          write ? BytesFrom(request.value) : Bytes{}, std::nullopt,
                          [manager, request, write](AttStatus status, Bytes value) {
      if (manager.state != CBManagerStatePoweredOn)
        return;
      if (!write && status == AttStatus::Success)
        request.value = Data(value);
      [manager respondToRequest:request withResult:static_cast<CBATTError>(status)];
    }});
  }

  void Failure(BluetoothError error) {
    if (std::exchange(failed_, true))
      return;
    if (completion_)
      Complete(Reply::Failure(std::move(error)));
    else
      events_(PeripheralFailure{request_.peripheral, std::move(error)});
  }

  void Complete(Reply reply) {
    if (auto completion = std::exchange(completion_, {}))
      completion(std::move(reply));
  }

  Request request_;
  Completion completion_;
  BackendEvents events_;
  HUXMacOSPeripheralDelegate* delegate_;
  CBPeripheralManager* manager_ = nil;
  std::deque<CBMutableService*> pending_;
  std::map<Token, Attribute> attributes_;
  bool started_ = false;
  bool failed_ = false;
};

namespace {

class MacOSPeripheralBackend final : public PeripheralBackend {
public:
  MacOSPeripheralBackend(BackendEvents events, bool configured)
      : events_(std::move(events)), configured_(configured) {}

  BluetoothCapabilities Capabilities() const override {
    return {.can_advertise = configured_, .can_use_gatt_server = configured_,
            .can_advertise_standalone = configured_};
  }

  Cancellation Invoke(Request request, Completion completion) override {
    if (CBManager.authorization != CBManagerAuthorizationAllowedAlways) {
      completion(Fail(Code::PermissionDenied, "Grant Bluetooth access before using Core Bluetooth"));
      return {};
    }
    if (request.operation == Operation::SendNotification) {
      const auto server = servers_.find(request.peripheral);
      if (server == servers_.end())
        completion(Fail(Code::Disconnected, "The local GATT server is closed"));
      else
        server->second->Notify(request.attribute, request.value, std::move(completion));
      return {};
    }
    const auto id = request.peripheral;
    const bool advertising = request.operation == Operation::Advertise;
    if (advertising && request.advertisement.connectable == false) {
      completion(Fail(Code::Unsupported, "Core Bluetooth does not expose nonconnectable service advertising"));
      return {};
    }
    auto& slots = advertising ? advertisements_ : servers_;
    if (!slots.empty()) {
      completion(Fail(Code::Busy, "A Core Bluetooth peripheral operation of this kind is already active"));
      return {};
    }
    auto pending = std::make_unique<MacOSPeripheralRole>(std::move(request), std::move(completion), events_);
    auto* role = pending.get();
    slots.emplace(id, std::move(pending));
    role->Open();
    return [this, id, advertising] {
      if (advertising)
        StopAdvertising(id);
      else
        CloseServer(id);
    };
  }

  void StopAdvertising(Token id) override {
    advertisements_.erase(id);
  }

  void CloseServer(Token id) override {
    servers_.erase(id);
  }

private:
  BackendEvents events_;
  bool configured_;
  std::map<Token, std::unique_ptr<MacOSPeripheralRole>> advertisements_;
  std::map<Token, std::unique_ptr<MacOSPeripheralRole>> servers_;
};

} // namespace

std::unique_ptr<PeripheralBackend> CreateMacOSPeripheral(BackendEvents events, bool configured) {
  return std::make_unique<MacOSPeripheralBackend>(std::move(events), configured);
}

} // namespace huxerui::bluetooth::detail

@implementation HUXMacOSPeripheralDelegate

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)manager {
  if (_owner)
    _owner->StateChanged(manager);
}

- (void)peripheralManager:(CBPeripheralManager*)manager didAddService:(CBService*)service error:(NSError*)error {
  if (_owner)
    _owner->Added(manager, service, error);
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)manager error:(NSError*)error {
  if (_owner)
    _owner->AdvertisingStarted(error);
}

- (void)peripheralManager:(CBPeripheralManager*)manager didReceiveReadRequest:(CBATTRequest*)request {
  if (_owner)
    _owner->Read(manager, request);
}

- (void)peripheralManager:(CBPeripheralManager*)manager didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
  if (_owner)
    _owner->Write(manager, requests);
}

- (void)peripheralManager:(CBPeripheralManager*)manager central:(CBCentral*)central
    didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
  if (_owner)
    _owner->Subscription(central, characteristic, true);
}

- (void)peripheralManager:(CBPeripheralManager*)manager central:(CBCentral*)central
    didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
  if (_owner)
    _owner->Subscription(central, characteristic, false);
}

@end
