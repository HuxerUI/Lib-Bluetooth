# HuxerUI Bluetooth Library Design

**Status** — Foreground BLE client implemented. Release support depends on platform device verification.

`HuxerUI::Bluetooth` gives HuxerUI applications a typed C++ API for discovering nearby devices, acquiring access, and exchanging data. The first public release covers all six current HuxerUI targets. Android, iOS, macOS, Windows, and Linux provide foreground BLE discovery and a GATT client. Web provides browser-managed device selection and the same GATT client workflow on supported browser and operating system combinations.

The public contract describes operations and capabilities. Each backend reports what the current host can do, including the effects of hardware, operating system version, packaging, and browser policy. The same contract can accommodate future HuxerUI targets.

## Release scope

| Capability | First public release | Extensions |
| --- | --- | --- |
| Adapter and access | Adapter status when observable, permission checks, and user-initiated access flows | Adapter selection on hosts that expose multiple adapters |
| Device discovery | Bounded foreground scanning on native platforms, browser device selection on Web, and device recovery where the host permits it | Additional scan filters and system accessory setup flows |
| Pairing | System-managed authentication for protected GATT operations, plus explicit pairing and bond queries on supported backends | Unpairing and platform-specific association management |
| BLE client | Connect, discover, read, write, subscribe, disconnect, and explicitly reconnect | Policy-driven reconnect, background communication, and state restoration |
| BLE peripheral | Capability reporting for advertising and GATT server operations | Advertising, local GATT services, and server-side request handling |
| Additional transports | The device model leaves room for transport-specific endpoints | Classic RFCOMM, applicable Apple accessory sessions, and LE L2CAP channels |

Qt Bluetooth provides separate interfaces for GATT and Bluetooth sockets. Capacitor BLE exposes continuous scanning and device selection as separate operations. FlutterBluePlus models service invalidation separately from connection state. These are useful boundaries for a library that spans native applications and browsers. [Qt Bluetooth](https://doc.qt.io/qt-6/qtbluetooth-index.html) · [Capacitor BLE](https://github.com/capacitor-community/bluetooth-le) · [FlutterBluePlus](https://pub.dev/packages/flutter_blue_plus)

## Public model

Expose the library through `<huxerui/bluetooth.h>` in `huxerui::bluetooth`. Register `bluetooth::Install` as an application hook and acquire the service with `UseService<bluetooth::Bluetooth>()`. The proposed declarations reuse HuxerUI's `Task`, `Result`, and `Bytes`. The excerpts omit private storage and backend factories.

| Type | Responsibility | Operations and observations |
| --- | --- | --- |
| `Bluetooth` | Application-scoped service | Query capabilities and adapter status, start a scan, select or recover a device, query bond state, pair, and open a GATT session. |
| `ScanSession` | One foreground scan | Observe device updates and termination. Stop explicitly or release the handle. |
| `Device` | Value identifying a device known to this backend | Carry an opaque identifier and available display information. A scan observation can also carry advertisement data. |
| `GattSession` | One logical connection to a remote GATT server | Observe connection state and service invalidation, discover attributes, read, write, subscribe, and disconnect. |
| `GattService`, `GattCharacteristic`, `GattDescriptor` | Discovered GATT objects | A service groups characteristics. Characteristics and descriptors carry a normalized UUID and an opaque session-scoped instance identity. |
| `GattSubscription` | One consumer's subscription to a characteristic | Receive remote value updates. Stop explicitly or release the handle. |

Use `Task` for operations that return one result and scoped handles for ongoing work. Deliver owned byte values so callers can safely retain data after a callback. A device value can outlive a scan. A connected session owns its platform connection resources until explicit close, handle release, or host shutdown.

Capabilities describe individual operations. Readiness combines those capabilities with the current adapter status, permission, and device access. An available operation can still require permission or an enabled radio. Use `Unknown` when the backend cannot establish the adapter status or bond state. At call time, return the actual outcome because conditions can change after a capability query.

## C++ API

### Values and errors

```cpp
namespace huxerui::bluetooth {

enum class BluetoothErrorCode {
  Unsupported,
  Unavailable,
  AdapterOff,
  PermissionDenied,
  AuthenticationRequired,
  AuthenticationFailed,
  Canceled,
  TimedOut,
  Busy,
  DeviceUnavailable,
  Disconnected,
  StaleAttribute,
  InvalidArgument,
  UnsupportedProperty,
  PayloadTooLarge,
  PlatformFailure,
};

struct BluetoothError {
  BluetoothErrorCode code;
  std::string message;
  std::string platform_domain;
  std::optional<std::int64_t> platform_code;
};

template <class T> using BluetoothResult = Result<T, BluetoothError>;

struct OperationOptions {
  std::chrono::milliseconds timeout = std::chrono::seconds{30};
};

enum class AdapterStatus { Unknown, Unavailable, PoweredOff, PoweredOn };
enum class BondState { Unknown, NotBonded, Bonding, Bonded };

struct BluetoothCapabilities {
  bool can_scan = false;
  bool can_select_device = false;
  bool can_restore_device = false;
  bool can_pair = false;
  bool can_query_bond = false;
  bool can_use_gatt_client = false;
  bool can_access_descriptors = false;
  bool can_advertise = false;
  bool can_use_gatt_server = false;
};

class Device final {
public:
  const std::string& Id() const;
  const std::optional<std::string>& Name() const;
  bool operator==(const Device& other) const noexcept;

private:
  Device();
};

} // namespace huxerui::bluetooth
```

UUID inputs accept 16-bit and 32-bit Bluetooth hexadecimal forms and canonical 128-bit UUIDs. Discovered UUIDs use the lowercase, hyphenated 128-bit form. Device equality compares backend-scoped identity. `Id()` is an opaque value that applications may save as a recovery hint. A `Device` comes from discovery, selection, or successful recovery.

`BluetoothResult<void>` represents success without a payload. A result's error code is stable across backends. Its message and platform fields are diagnostics. For ordinary asynchronous operations, the timeout starts when the Task starts, covers queueing and execution, and must be positive. The scan duration and the browser chooser have their own lifetime rules.

### Discovery and device selection

```cpp
namespace huxerui::bluetooth {

struct ScanOptions {
  std::vector<std::string> advertised_services;
  std::chrono::milliseconds duration = std::chrono::seconds{10};
};

struct SelectDeviceOptions {
  std::vector<std::string> advertised_services;
  std::vector<std::string> optional_services;
  bool accept_all_devices = false;
};

struct Advertisement {
  std::optional<std::string> local_name;
  std::vector<std::string> service_uuids;
  std::map<std::uint16_t, Bytes> manufacturer_data;
  std::map<std::string, Bytes> service_data;
  std::optional<int> rssi_dbm;
  std::optional<bool> connectable;
};

struct ScanObservation {
  Device device;
  Advertisement advertisement;
};

enum class ScanEndReason { DurationElapsed, Stopped };
using ScanEvent = std::variant<ScanObservation, ScanEndReason, BluetoothError>;

class ScanSession final {
public:
  ScanSession(ScanSession&&) noexcept;
  ScanSession& operator=(ScanSession&&) noexcept;
  ~ScanSession();

  void Stop() noexcept;

private:
  ScanSession();
};

} // namespace huxerui::bluetooth
```

An empty scan filter matches all advertisements. A nonempty filter matches any listed advertised service. The chooser requires a nonempty service filter or an explicit `accept_all_devices` with no filter. `optional_services` grants access to additional GATT services if the user selects a device. An advertised service filter can exclude a device whose GATT database contains that service but whose advertisements omit it.

`StartScan` returns after admitting the request. It reports a later backend start failure as the terminal `BluetoothError` event. While the caller retains the handle, explicit stop or expiration produces one terminal event and closes the scan slot. Releasing the handle stops the scan and suppresses queued callbacks. A duration that elapses normally produces `DurationElapsed`.

### GATT attributes and session

```cpp
namespace huxerui::bluetooth {

enum class GattWriteMode { WithResponse, WithoutResponse };

struct GattCharacteristicProperties {
  bool can_read = false;
  bool can_write_with_response = false;
  bool can_write_without_response = false;
  bool can_notify = false;
  bool can_indicate = false;
};

class GattDescriptor final {
public:
  const std::string& Uuid() const;
  bool operator==(const GattDescriptor& other) const noexcept;

private:
  GattDescriptor();
};

class GattCharacteristic final {
public:
  const std::string& Uuid() const;
  GattCharacteristicProperties Properties() const;
  const std::optional<std::vector<GattDescriptor>>& Descriptors() const;
  bool operator==(const GattCharacteristic& other) const noexcept;

private:
  GattCharacteristic();
};

struct GattService {
  std::string uuid;
  std::vector<GattCharacteristic> characteristics;
};

enum class GattConnectionState { Connected, Disconnecting, Disconnected };

struct GattServicesChanged {};
struct GattDisconnected {
  std::optional<BluetoothError> error;
};
using GattEvent = std::variant<GattServicesChanged, GattDisconnected>;

class GattSubscription final {
public:
  GattSubscription(GattSubscription&&) noexcept;
  GattSubscription& operator=(GattSubscription&&) noexcept;
  ~GattSubscription();

  void Stop() noexcept;

private:
  GattSubscription();
};

class GattSession final {
public:
  GattSession(GattSession&&) noexcept;
  GattSession& operator=(GattSession&&) noexcept;
  ~GattSession();

  GattConnectionState State() const;
  std::optional<std::size_t> MaxWriteSize(GattWriteMode mode) const;

  Task<BluetoothResult<std::vector<GattService>>> DiscoverAsync(OperationOptions options = {});
  Task<BluetoothResult<Bytes>> ReadAsync(GattCharacteristic characteristic, OperationOptions options = {});
  Task<BluetoothResult<void>> WriteAsync(GattCharacteristic characteristic, Bytes value, GattWriteMode mode,
                                        OperationOptions options = {});
  Task<BluetoothResult<GattSubscription>> SubscribeAsync(GattCharacteristic characteristic,
                                                        std::function<void(Bytes)> on_value,
                                                        OperationOptions options = {});
  Task<BluetoothResult<Bytes>> ReadDescriptorAsync(GattDescriptor descriptor, OperationOptions options = {});
  Task<BluetoothResult<void>> WriteDescriptorAsync(GattDescriptor descriptor, Bytes value,
                                                  OperationOptions options = {});

  void Disconnect() noexcept;

private:
  GattSession();
};

} // namespace huxerui::bluetooth
```

`ScanSession`, `GattSession`, and `GattSubscription` are move-only. A session's attributes are copyable values with identity scoped to that session and discovery snapshot. An absent descriptor list means enumeration was unavailable. An empty list means enumeration succeeded and the characteristic had no descriptors. Discovery reports services visible to the application, including the browser's service grant restrictions.

`GattServicesChanged` retires the active discovery snapshot and its subscriptions. A successful rediscovery replaces the snapshot and also retires any previous handles and subscriptions. `SubscribeAsync` enables delivery before it succeeds. An update received during setup can be delivered on the dispatcher after the awaiting caller has received its subscription handle. `Disconnect` retires the session for new I/O immediately. Its terminal event arrives once the connection slot can admit another attempt. `GattDisconnected::error` is empty for an intentional close and carries the available error for unexpected loss.

### Application service

```cpp
namespace huxerui::bluetooth {

class Bluetooth final {
public:
  BluetoothCapabilities Capabilities() const;
  AdapterStatus GetAdapterStatus() const;
  Task<void> WatchAdapterStatusAsync(std::function<void(AdapterStatus)> on_status) const;

  BluetoothResult<ScanSession> StartScan(ScanOptions options, std::function<void(ScanEvent)> on_event);
  Task<BluetoothResult<Device>> SelectDevice(SelectDeviceOptions options);
  Task<BluetoothResult<Device>> RestoreDeviceAsync(std::string id, OperationOptions options = {});

  Task<BluetoothResult<BondState>> GetBondStateAsync(Device device, OperationOptions options = {});
  Task<BluetoothResult<void>> PairAsync(Device device, OperationOptions options = {});
  Task<BluetoothResult<GattSession>> ConnectAsync(Device device, OperationOptions options = {},
                                                 std::function<void(GattEvent)> on_event = {});

private:
  Bluetooth();
};

void Install(ApplicationContext& context);

} // namespace huxerui::bluetooth
```

Call service and session operations on the owning application thread. `WatchAdapterStatusAsync` emits the current status and then distinct changes until its HuxerUI task scope is canceled or the service closes. Start pairing from an explicit user action on backends that support it. `PairAsync` completes when the host confirms pairing or reports the device already paired. Check `GetBondStateAsync` when the UI needs the separately observable bond state. `RestoreDeviceAsync` re-resolves a saved identifier through the backend's known-device or grant API. It can recover a device that is currently out of range.

`SelectDevice` starts the chooser as part of the initiating call and returns a Task that waits for its outcome. Start it inside the actual user action and move the Task into the caller's task scope. If the caller cancels and the host cannot dismiss the chooser, the library drops delivery to that caller and releases the chooser slot when the host settles it. The other Task-returning operations start when awaited or launched. A canceled HuxerUI task scope detaches its continuation. A platform cancellation can still be returned as `Canceled` to a live caller.

### Application examples

A device list backed by scanning connects its event sink before the scan starts. Retain the returned `ScanSession` while the list owns discovery.

```cpp
auto bluetooth = huxerui::UseService<huxerui::bluetooth::Bluetooth>();
auto scan = bluetooth->StartScan({.advertised_services = {required_service}}, on_scan_event);
```

For a browser chooser, call `SelectDevice` directly in the click handler. The callback receives the selected `Device` or its error in the owning UI scope.

```cpp
auto selection = bluetooth->SelectDevice({.advertised_services = {required_service}});
tasks.Launch([selection = std::move(selection), on_selected]() mutable -> huxerui::Task<void> {
  on_selected(co_await std::move(selection));
});
```

Once the application has a device, the GATT path is common. In this example, a fixture exposes exactly one matching service and characteristic. The application retains the connection and subscription handles in its model.

```cpp
auto connected = co_await bluetooth->ConnectAsync(device, {}, on_gatt_event);
if (!connected.Succeeded()) {
  report_error(connected.Error());
  co_return;
}
auto session = std::move(connected).Value();

auto discovered = co_await session.DiscoverAsync();
if (!discovered.Succeeded()) {
  report_error(discovered.Error());
  co_return;
}
auto characteristic = find_fixture_characteristic(discovered.Value());
if (!characteristic) {
  report_missing_characteristic();
  co_return;
}

auto subscribed = co_await session.SubscribeAsync(*characteristic, on_value);
if (!subscribed.Succeeded()) {
  report_error(subscribed.Error());
  co_return;
}
keep_connection(std::move(session), std::move(subscribed).Value());
```

## Device discovery and identity

Foreground scans run for a finite duration. The application can stop a scan when it has found the target or when the owning UI closes. The service owns one active foreground scan per Runtime in the first release. Starting another returns `Busy`, and a stopped or expired scan releases the slot. This gives multiple windows predictable ownership without introducing scan merging.

- The common scan filter matches advertised service UUIDs. A device can expose a service after connecting even when its advertisements omit that UUID. Applications that identify a device by other means can request an unfiltered foreground scan and inspect available advertisement data.
- Scan observations can include a local name, advertised service UUIDs, manufacturer data keyed by company identifier, service data keyed by service UUID, RSSI, and connectability. Represent unavailable fields explicitly. Use repeated observations to refresh known devices and bound delivery to the UI when advertisements arrive faster than it can render.
- Deduplicate observations using the backend device identifier. Devices may rotate addresses, Apple uses platform identifiers, and browser device access is scoped to the origin. Treat the identifier as opaque and scoped to its backend. Offer explicit recovery only where the host can re-resolve previously known or granted devices. Persisted references may become invalid after a permission change, reinstall, device reset, or browser data removal.

On Web, device selection calls `navigator.bluetooth.requestDevice()` within the initiating user gesture. Start the browser request before an asynchronous bridge hop can lose activation, then return an awaitable result through the ordinary HuxerUI dispatcher. Declare the required service UUIDs in the filters or `optionalServices`. A completed selection provides a device grant and the selected device can then open a GATT session. Use `getDevices()` for recovery where the browser exposes it. [Chrome Web Bluetooth guide](https://developer.chrome.com/docs/capabilities/bluetooth) · [Web Bluetooth implementation status](https://github.com/whatwg/bluetooth/blob/main/implementation-status.md)

## Authorization and pairing

Keep three states independent.

| State | Meaning | How it changes |
| --- | --- | --- |
| Application access | The application can use Bluetooth or access a specific device. | OS permission, browser selection, accessory setup, or a host policy change. |
| Bond | The OS and device have established a reusable Bluetooth security relationship when the host exposes that state. | Explicit pairing, system authentication during a protected operation, or removal of the bond. |
| Connection | The application owns a live logical GATT session. | Connect, disconnect, remote link loss, or host shutdown. |

Use `ApplicationHandle` and the SDK's `BluetoothScan` and `BluetoothConnect` permissions on Android, iOS, and macOS. The application chooses when to request access and supplies package declarations, usage descriptions, and entitlements. Windows and Linux take access decisions from their Bluetooth backends. Web device grants come from the browser. BlueZ pairing may use an available system agent, and Windows pairing uses the host's pairing ceremony. Explicit pairing completes on a reported terminal result, with cancellation or failure exposed when the host can distinguish it. [Android Bluetooth permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions) · [Windows pairing](https://learn.microsoft.com/windows/uwp/devices-sensors/pair-devices) · [BlueZ device API](https://bluez.readthedocs.io/en/latest/device-api/)

Ordinary BLE devices can connect without a bond. A device can require authentication or encryption when a particular value is read, written, or subscribed to. The operating system owns any resulting security prompt and the library propagates the final operation result. Keep `Pair` optional and keep `Unpair` distinct from removing application access or forgetting a saved device reference. Apple's AccessorySetupKit can provide a system picker and pairing flow for eligible accessories on iOS 18 and later. It is a candidate for accessory setup alongside the Core Bluetooth path available to the current iOS target. [Apple Core Bluetooth](https://developer.apple.com/documentation/corebluetooth) · [Apple AccessorySetupKit sample](https://developer.apple.com/documentation/accessorysetupkit/setting-up-and-authorizing-a-bluetooth-accessory)

## Connection and GATT lifecycle

### Logical connection

Open a session from a discovered or recovered device. A successful connect means the backend has established access sufficient to begin GATT work. Returning a platform device object alone does not establish that condition. On Windows the backend can use an uncached discovery operation or a GATT session to drive connection establishment. The OS can share the physical link with other clients, so `Disconnect` closes this library's logical session and releases its resources. [Windows GATT client](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/gatt-client)

A connection attempt has a deadline and can be canceled. Support concurrent sessions for distinct devices subject to host capacity. Return `Busy` for a second connection to a device with an active session. Represent intentional closure and unexpected loss as terminal session events. An explicit reconnect opens a new session with fresh attribute handles. The caller can then resolve the services and reestablish subscriptions its device protocol requires.

### Attribute discovery and invalidation

Discover services and their characteristics, including characteristic properties and available descriptors. Normalize UUIDs for comparison and preserve the instance of each discovered object. The same service or characteristic UUID can appear more than once. When a lookup is ambiguous, expose the matches so callers can select the intended instance.

An attribute handle belongs to one session and one discovery snapshot. When the backend reports a service database change, retire the snapshot, fail pending operations that reference it, and signal that discovery is required again. Hosts vary in the service-change information they deliver. Explicit rediscovery lets the application refresh attributes when that change is detected through another path. [Bluetooth SIG GATT specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/host/generic-attribute-profile--gatt-.html) · [Core Bluetooth service invalidation](https://developer.apple.com/documentation/corebluetooth/cbperipheraldelegate/peripheral(_:didmodifyservices:))

### Reads, writes, and subscriptions

| Operation | Completion contract |
| --- | --- |
| Read | Returns an owned byte value or an operation error. |
| Write with response | Completes successfully when the GATT operation is acknowledged. Device-level command execution can be confirmed by the application protocol. |
| Write without response | Completes when the backend accepts the value for transmission. Apply platform flow control before accepting more data and keep the pending queue bounded. |
| Subscribe | Completes when the backend enables notifications or indications for the characteristic. The subscription then delivers remote value updates until stopped or invalidated. |
| Descriptor access | Reads or writes descriptors supported by the host and device. The subscription implementation owns Client Characteristic Configuration Descriptor changes. |

Expose the maximum single write payload for each write mode where the backend can determine it. Validate known limits before dispatch and use host errors when the limit is unknown. Separate the link's negotiated MTU from the payload limit exposed by a platform API. Long transfers belong to the device protocol and can choose framing, chunk size, checksums, retransmission, and progress. [Android GATT API](https://developer.android.com/reference/android/bluetooth/BluetoothGatt) · [Apple write types and payload limits](https://developer.apple.com/documentation/corebluetooth/cbperipheral/writevalue%28_%3Afor%3Atype%3A%29) · [Apple write flow control](https://developer.apple.com/documentation/corebluetooth/cbperipheral/cansendwritewithoutresponse)

On one characteristic, several subscribers can share the same platform registration. Enable it for the first subscriber and disable it when the last subscriber stops. The iOS, macOS, and BlueZ backends report `Busy` for reads of a subscribed characteristic because their value delivery can overlap with read completion. Stop the subscription before reading that characteristic on these backends.

## Execution, errors, and cleanup

Register through the existing `bluetooth::Install(ApplicationContext&)` hook and expose one application-scoped service through `ApplicationContext::Provide` and `UseService`. Open the backend lazily. The Runtime owns the service, while the application's model or UI scope owns each session. Platform adapters keep platform objects and platform channels internal. Use a typed PlatformModule at a cross-language boundary where it fits the host integration. [Public header](../include/huxerui/bluetooth.h) · [Preview application](../examples/preview/src/app.cpp)

Deliver results and events on the owning HuxerUI dispatcher. Serialize operations per session where the platform requires ordered admission. A canceled or timed-out operation completes for the caller, while a platform operation already in progress may still settle. Track its request and session generation until the backend drains it or retires the connection. A late callback must never complete a later request. If the library cannot establish a safe point for further I/O, close the session and surface the reason so the caller can reconnect.

Stopping a scan, closing a connection, or releasing a subscription is idempotent. When adapter availability or access changes, retire affected work and notify observers. On Runtime shutdown, release platform watchers, connections, event listeners, and delegates. Operations awaiting a result either complete with a terminal error while their owner is alive or detach when their task scope is canceled.

| Error category | Caller action |
| --- | --- |
| `Unsupported`, `Unavailable`, `AdapterOff` | Adjust the feature path, check host configuration or hardware, or ask the user to enable Bluetooth. |
| `PermissionDenied`, `AuthenticationRequired`, `AuthenticationFailed` | Request the appropriate access or guide the user through the system security flow when available. |
| `Canceled`, `TimedOut`, `Busy` | Present the operation's actual outcome and let the user or application decide whether to retry. |
| `DeviceUnavailable`, `Disconnected`, `StaleAttribute` | Recover the device, reconnect, or discover attributes again as appropriate. |
| `InvalidArgument`, `UnsupportedProperty`, `PayloadTooLarge` | Correct the request, select a compatible characteristic or write mode, or use device-protocol framing. |
| `PlatformFailure` | Preserve available platform status and operation context for diagnosis. |

Keep distinctions only when the host exposes them. For example, a chooser can report the same outcome for dismissal and failure to select a device. Retain the available diagnostic information without inventing a cause. Log operation and status by default. Leave device identifiers, manufacturer data, and application payloads out of routine logs.

## Platform backends

| Target | First-release backend | Optional paths and limits |
| --- | --- | --- |
| Android | `BluetoothLeScanner`, `BluetoothGatt`, and public `createBond()` with bond state events. Request the permissions appropriate to the OS and target SDK. | GATT server, BLE advertising, RFCOMM, and LE L2CAP. Capabilities depend on adapter and API version. The application's `neverForLocation` setting can affect beacon visibility. [Permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions) · [BluetoothDevice](https://developer.android.com/reference/android/bluetooth/BluetoothDevice) · [GATT server](https://developer.android.com/reference/android/bluetooth/BluetoothGattServer) |
| iOS | `CBCentralManager` and `CBPeripheral`. Protected GATT access uses Core Bluetooth security. The application supplies `NSBluetoothAlwaysUsageDescription`. | AccessorySetupKit on eligible devices, `CBPeripheralManager` for local services and advertising, and LE L2CAP. External Accessory serves applicable MFi protocols. [Core Bluetooth](https://developer.apple.com/documentation/corebluetooth) · [Peripheral manager](https://developer.apple.com/documentation/corebluetooth/cbperipheralmanager) · [External Accessory](https://developer.apple.com/documentation/externalaccessory/) |
| macOS | Core Bluetooth central and GATT client. The application supplies the relevant usage description and, when sandboxed, Bluetooth entitlement. | Core Bluetooth peripheral APIs and an independent IOBluetooth RFCOMM backend. [Bluetooth entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.device.bluetooth) · [IOBluetooth RFCOMM](https://developer.apple.com/documentation/iobluetooth/iobluetoothrfcommchannel) |
| Windows | WinRT advertisement watcher, GATT client, and device pairing on supported desktop versions. Test with the actual Win32 Preview packaging. | GATT server requires an appropriate OS version and adapter. RFCOMM uses its own transport. The Windows 7 compatibility build reports BLE as `Unsupported`. [Windows Bluetooth capabilities](https://learn.microsoft.com/windows-hardware/design/component-guidelines/bluetooth) · [Win32 GATT requirements](https://learn.microsoft.com/windows/win32/api/bluetoothleapis/nf-bluetoothleapis-bluetoothgattgetservices) · [RFCOMM](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/send-or-receive-files-with-rfcomm) |
| Linux | BlueZ over the system D-Bus, using adapter discovery, device pairing and connection, and GATT objects. Observe `ServicesResolved` before exposing attributes. | Advertisements, local GATT services, RFCOMM profiles, and multiple adapters where available. Detect missing BlueZ, missing adapters, and D-Bus access failures. [BlueZ adapter](https://bluez.readthedocs.io/en/latest/adapter-api/) · [BlueZ device](https://bluez.readthedocs.io/en/latest/device-api/) · [BlueZ GATT](https://bluez.readthedocs.io/en/latest/gatt-api/) |
| Web | `navigator.bluetooth` for selection and GATT access. Check browser support, secure context, user activation, and Permissions Policy where applicable. | Previously granted devices can be recovered when the API is exposed. The first-release browser backend reports continuous scanning, explicit bonding, GATT server, and RFCOMM as `Unsupported`. [Chrome Web Bluetooth guide](https://developer.chrome.com/docs/capabilities/bluetooth) · [Web Bluetooth implementation status](https://github.com/whatwg/bluetooth/blob/main/implementation-status.md) |

Keep the existing `HuxerUI::Bluetooth` CMake target and the Preview's `huxerui_use_library` edge. Use the SDK library graph for the Android Gradle module and iOS Swift package. Desktop and Web backends follow the platform source selection in CMake. Supply the Preview's permissions and privacy metadata in its application packages. Preserve the graph-only configuration path when adding platform dependencies. [Library build](../CMakeLists.txt) · [Preview integration](../examples/preview/CMakeLists.txt) · [Android library](../platform/android/build.gradle) · [iOS package](../platform/ios/Bluetooth/Package.swift)

iOS and macOS compile their own Core Bluetooth implementations. The shared C++ layer owns the application API and operation lifecycle across all platforms. The Windows GATT backend requires the asynchronous GATT APIs introduced with Windows 10 version 1703. [iOS backend](../platform/ios/src/bluetooth.mm) · [macOS backend](../platform/macos/src/bluetooth.mm) · [Windows BluetoothLEDevice API](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice)

## Extension contracts

### Advertising and local GATT

Expose advertising and GATT server as independent capabilities. An advertising session owns its lifetime and reports when the platform actually starts advertising. Its options describe requested service UUIDs, names, manufacturer data, connectability, and payload size only where those fields are supported. Core Bluetooth constrains the fields available for general advertising. Return an explicit result when a requested option is unavailable. [Apple advertising API](https://developer.apple.com/documentation/corebluetooth/cbperipheralmanager/startadvertising(_:)) · [BlueZ advertising API](https://bluez.readthedocs.io/en/latest/advertising-api/)

A GATT server owns registered services, characteristic and descriptor definitions, security requirements, per-peer subscriptions, and server-side read and write callbacks. Respond to requests within the host's deadline. Support peer targeting and backpressure when sending notifications or indications. Test radio capacity, concurrent roles, and host-specific attribute restrictions before publishing support. [Windows GATT server](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/gatt-server) · [BlueZ GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/)

### Classic Bluetooth and other transports

RFCOMM exposes classic service discovery by UUID, an outbound byte stream, and explicit close. Listener and inbound connection support can be added where the host provides them. Classic serial printers, modules, and scanners are candidate clients. LE L2CAP uses a channel endpoint and separate capability. Apple External Accessory uses the accessory protocol declared by the application. Device profiles such as audio, keyboard input, and MIDI belong to the relevant OS services or application libraries. [Qt Bluetooth socket](https://doc.qt.io/qt-6/qbluetoothsocket.html) · [BlueZ profile API](https://bluez.readthedocs.io/en/latest/profile-api/) · [Core Bluetooth L2CAP](https://developer.apple.com/documentation/corebluetooth/cbperipheral/openl2capchannel(_:))

### Reconnect and background operation

Define foreground reconnect, background communication, and process restoration separately. An opt-in reconnect policy owns its retry budget, backoff, and cancellation. It reacquires the device, opens a fresh session, and allows the caller to reconcile its subscriptions and the device's state. Retrying commands requires the application protocol's rules for idempotency.

Background capabilities depend on the host lifecycle and app configuration. Android provides mechanisms such as companion-device integration and foreground services for eligible workloads. iOS has Core Bluetooth background modes and state preservation and restoration for eligible workloads. Feature support describes the exact work and lifecycle the backend can sustain. [Android background BLE](https://developer.android.com/develop/connectivity/bluetooth/ble/background) · [Apple background processing and restoration](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html)

## Implementation and acceptance

Build the service, value types, operation state, and error mapping around the shared contract. Use a controllable backend in tests for cancellation, timeouts, and delayed callbacks. Add and validate the six production backends against the existing Preview. The Preview should expose the host's actual capabilities and provide a device picker or scan list, a session status view, attribute discovery, and raw byte read, write, and subscription controls.

| Verification | Acceptance condition |
| --- | --- |
| Foreground scan and GATT | The packaged Preview scans, connects, discovers, reads, performs both supported write modes, subscribes, disconnects, and reconnects on Android, iOS, macOS, Windows, and Linux. Use a peripheral that exposes the corresponding properties. |
| Browser happy path | In a supported browser and host combination, a user action opens the chooser and the selected device runs the same GATT flow. Validate access to explicitly declared services and the unsupported-browser experience. |
| Protected attributes | An encrypted or authenticated test characteristic produces the system flow and the correct result. Validate an explicit pair and bond query on Android, Windows, and Linux when the host supports them. |
| Discovery and lifetime | Validate scan timeout and cancellation, repeated advertisements, duplicate attribute UUIDs, unexpected disconnect, service invalidation, and fresh handles after reconnect. |
| Concurrency and failure | Validate two devices, a duplicate connect attempt, disconnect with I/O pending, a late platform callback, subscription release, write flow control, and adapter or permission loss. |
| Build and packaging | Build the actual target packages and verify permissions, entitlements, platform library linking, and unsupported capability feedback. Record the OS or browser version and adapter used for each device run. |

Use one known BLE test profile with read, write, and update characteristics, plus a configuration or second peripheral with protected attributes. A backend's release support follows successful device verification on that platform. Unsupported options have observable results in the same application flow.
