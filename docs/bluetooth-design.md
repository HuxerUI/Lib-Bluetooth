# HuxerUI Bluetooth Library Design

**Status** — Foreground BLE client and peripheral operations, outbound RFCOMM, and outbound LE L2CAP implemented. Release support depends on platform device verification.

`HuxerUI::Bluetooth` gives HuxerUI applications a typed C++ API for discovering nearby devices, acquiring access, and exchanging data. The first public release covers all six current HuxerUI targets. Android, iOS, macOS, Windows, and Linux provide foreground BLE discovery and a GATT client. Web provides browser-managed device selection and the same GATT client workflow on supported browser and operating system combinations.

The public contract describes operations and capabilities. Each backend reports what the current host can do, including the effects of hardware, operating system version, packaging, and browser policy. The same contract can accommodate future HuxerUI targets.

## Release scope

| Capability | First public release | Extensions |
| --- | --- | --- |
| Adapter and access | Adapter status when observable, permission checks, and user-initiated access flows | Adapter selection on hosts that expose multiple adapters |
| Device discovery | Bounded foreground scanning on native platforms, browser device selection on Web, and device recovery where the host permits it | Additional scan filters and system accessory setup flows |
| Pairing | System-managed authentication for protected GATT operations, plus explicit pairing and bond queries on supported backends | Unpairing and platform-specific association management |
| BLE client | Connect, discover, read, write, subscribe, disconnect, and recover a foreground connection with its subscriptions | Background communication and process restoration |
| BLE peripheral | Foreground advertising and local GATT read, write with response, and notifications where supported | Additional advertising fields, custom descriptors, access protection, and peer-targeted updates |
| Bluetooth channels | Outbound RFCOMM by service UUID and LE L2CAP by PSM where supported | Incoming channels and applicable Apple accessory sessions |

Qt Bluetooth provides separate interfaces for GATT and Bluetooth sockets. Capacitor BLE exposes continuous scanning and device selection as separate operations. FlutterBluePlus models service invalidation separately from connection state. These are useful boundaries for a library that spans native applications and browsers. [Qt Bluetooth](https://doc.qt.io/qt-6/qtbluetooth-index.html) · [Capacitor BLE](https://github.com/capacitor-community/bluetooth-le) · [FlutterBluePlus](https://pub.dev/packages/flutter_blue_plus)

## Public model

Expose the library through `<huxerui/bluetooth.h>` in `huxerui::bluetooth`. Register `bluetooth::Install` as an application hook and acquire the service with `UseService<bluetooth::Bluetooth>()`. The proposed declarations reuse HuxerUI's `Task`, `Result`, and `Bytes`. The excerpts omit private storage and backend factories.

| Type | Responsibility | Operations and observations |
| --- | --- | --- |
| `Bluetooth` | Application-scoped service | Query capabilities and adapter status, start a scan, select or recover a device, query bond state, pair, and open GATT sessions or Bluetooth channels. |
| `ScanSession` | One foreground scan | Observe device updates and termination. Stop explicitly or release the handle. |
| `Device` | Value identifying a device known to this backend | Carry an opaque identifier and available display information. A scan observation can also carry advertisement data. |
| `GattSession` | One logical connection to a remote GATT server | Observe connection state and service invalidation, discover attributes, read, write, subscribe, and disconnect. |
| `BluetoothChannel` | One logical RFCOMM or LE L2CAP byte stream | Read, write, and close, with optional terminal notification. |
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
  bool can_advertise_standalone = false;
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

Call service and session operations on the owning application thread. `WatchAdapterStatusAsync` emits the current status and then distinct changes until its HuxerUI task scope is canceled or the service closes. Start pairing from an explicit user action on backends that support it. `PairAsync` completes when the host confirms pairing or reports the device already paired. Check `GetBondStateAsync` when the UI needs the separately observable bond state. `RestoreDeviceAsync` re-resolves a saved identifier through the backend's known-device or grant API. It can recover a device that is currently out of range. See the connection helper for `ConnectWithRetryAsync()`. [Connection helper](#connection-helper)

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

## Device management and foreground reconnection

An application can remember a selected device, recover it when it next opens, and resume the GATT subscriptions required by its device protocol after a connection loss. Use `RestoreDeviceAsync()` for device identity and `ConnectWithRetryAsync()` for the opt-in retry and setup sequence.

### Saved devices

Save a copy of `Device::Id()` when the user chooses a device from a scan or system chooser. The application owns the saved record and can also keep a display name and the association with its device profile. The identifier is scoped to its backend. On startup, load the saved identifier and call `RestoreDeviceAsync()` when `can_restore_device` is available.

A successful recovery returns a `Device` that the backend currently recognizes. Bluetooth authorization, the system bond, and reachability are established by their respective operations. A device can be recognized while it is out of range. A failed recovery leaves the saved record available to the application. When recovery is unavailable or the backend no longer recognizes the identifier, the user can select the device again and update the saved record. Retry connection attempts against an already resolved `Device`.

The saved device list represents the user's selections in this application. Removing a saved record removes the application's preference. Use `GetBondStateAsync()` where supported when the UI also needs the system bond. Platform-authorized device enumeration, unpairing, and access revocation remain independent operations with their own availability.

On Web, the existing recovery path looks up the identifier among devices previously granted to the site when the browser exposes that API. Start a new chooser through `SelectDevice()` from a user gesture when a new grant is needed. On iOS and macOS, Core Bluetooth resolves identifiers against known peripherals. The application still uses its permission and connection results to determine current access. [Web Bluetooth device access](https://developer.chrome.com/docs/capabilities/bluetooth) · [Core Bluetooth known-device lookup](https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/retrieveperipherals(withidentifiers:))

### Connection helper

The operation is a lazy Task with a bounded number of attempts. It can open a connection to a recovered device at startup or rebuild a connection after an unexpected disconnect. The library admits one pending recovery per device. The result groups the existing move-only handles with the services discovered for that session.

```cpp
struct ReconnectOptions {
  std::size_t max_attempts = 3;
  std::chrono::milliseconds initial_retry_delay{250};
  std::chrono::milliseconds max_retry_delay{2000};
  OperationOptions operation;
};

struct GattConnection {
  GattSession session;
  std::vector<GattService> services;
  std::vector<GattSubscription> subscriptions;
};

using RestoreSubscriptions = std::function<Task<BluetoothResult<std::vector<GattSubscription>>>(
    GattSession&, std::span<const GattService>)>;

// Member of Bluetooth.
Task<BluetoothResult<GattConnection>> ConnectWithRetryAsync(
    Device device, ReconnectOptions options = {}, RestoreSubscriptions restore_subscriptions = {},
    std::function<void(GattEvent)> on_event = {});
```

Each attempt calls `ConnectAsync()`, then `DiscoverAsync()`, then the optional `restore_subscriptions` Task. An empty callback yields a connection and discovered services with an empty subscription list. `operation` supplies the existing per-operation deadline for connection and discovery. The application's subscription setup can pass `OperationOptions` to each `SubscribeAsync()` call.

The first attempt starts immediately when the Task begins in the foreground. `max_attempts` counts the first attempt and must be positive. Before a retry, delay by `initial_retry_delay` and double the delay for each subsequent retry up to `max_retry_delay`. Both delays must be positive and the maximum must be at least the initial delay. Invalid options, including a nonpositive `operation.timeout`, return `InvalidArgument` before the first attempt. The attempt limit applies to the full connection and setup sequence.

Retry `DeviceUnavailable`, `Disconnected`, and `TimedOut` during connection or setup. A snapshot invalidated during setup can use a retry when the current attempt observed a service database change. Report the last error when the retry budget is exhausted. Other errors end the batch. This includes `Busy`, which can mean that another operation already owns the device. The application can use existing permission and adapter APIs to decide when to start a new batch.

### Subscription setup and ownership

`RestoreSubscriptions` receives the candidate session and its GATT snapshot. The helper owns both throughout setup, and the callback borrows them until its returned Task completes. Subscription setup runs in the application task scope. The callback resolves its targets using the current snapshot, calls `SubscribeAsync()`, and returns the owners for the required subscriptions. An application can handle an optional subscription failure inside the callback and include its remaining subscriptions in the result.

Resolve by service and characteristic UUID when that pair is unique. If the same UUID appears more than once, the device profile must identify the intended instance from the new snapshot or return an error. This keeps attribute selection tied to the application protocol. A missing required characteristic can return `UnsupportedProperty`, and an ambiguous selection can return `InvalidArgument`.

An attempt publishes a successful result after connection, discovery, and required subscription setup succeed on the same snapshot. During setup, the helper observes disconnect and service-change events so it can reject a retired candidate. A failed or canceled attempt releases its subscriptions and closes its session before another attempt starts. After handoff, the application's `on_event` receives events from the returned session and the application owns its lifetime.

Value callbacks follow the existing `SubscribeAsync()` delivery contract. A subscription can begin delivering after it is enabled while the application is still setting up another subscription. Applications that need to delay updates until their model is ready can gate their handlers on that model. Replaying a business command after recovery follows the device protocol's idempotency rules.

### Foreground lifecycle

Start the helper while the application is active. A temporary inactive transition lets an already admitted attempt finish. If the initial attempt or a retry reaches admission while the application is inactive, end the batch with `Canceled`. Entering the background retires any pending batch and drops late results. A new batch on return to the foreground is an application decision. Connections that have already been handed to the application retain the existing `GattSession` lifetime contract.

Run the helper in the application's chosen `TaskScope`. Keep its `TaskHandle` while recovery is pending and cancel it when the user disconnects, changes devices, or closes the owning view. HuxerUI Task cancellation detaches the caller's continuation. A batch retired by the library's foreground rule returns `Canceled` to a live caller. The connection helper uses the existing operations without requesting a chooser or starting an explicit pairing flow. System access or authentication behavior during a platform operation still applies.

After an intentional disconnect, leave recovery idle. After an unexpected disconnect eligible for retry, the application can schedule the helper from its existing `GattDisconnected` handler, using the retained `Device`. The event arrives after the library retires the old connection slot. Clear the retired session and subscription owners before starting the next batch. Track one recovery Task in the application model so a repeated event or user click cannot start another batch for the same device.

### Application flow

On startup the application resolves its saved device and passes it to the connection helper. The same subscription callback can be used on startup and after an unexpected disconnect.

```cpp
auto restored = co_await bluetooth->RestoreDeviceAsync(saved_device_id);
if (!restored.Succeeded()) {
  ShowDeviceRecoveryError(restored.Error());
  co_return;
}

auto device = std::move(restored).Value();
auto connection = co_await bluetooth->ConnectWithRetryAsync(
    device, {}, RestoreDeviceProfileSubscriptions, OnGattEvent);
if (connection.Succeeded()) {
  StoreConnection(std::move(device), std::move(connection).Value());
} else {
  ShowConnectionError(connection.Error());
}
```

```cpp
Task<BluetoothResult<std::vector<GattSubscription>>> RestoreDeviceProfileSubscriptions(
    GattSession& session, std::span<const GattService> services) {
  auto characteristic = FindDeviceProfileCharacteristic(services);
  if (!characteristic.Succeeded()) {
    co_return BluetoothResult<std::vector<GattSubscription>>::Failure(
        std::move(characteristic).Error());
  }

  auto subscribed = co_await session.SubscribeAsync(
      std::move(characteristic).Value(), ConsumeDeviceUpdate);
  if (!subscribed.Succeeded()) {
    co_return BluetoothResult<std::vector<GattSubscription>>::Failure(
        std::move(subscribed).Error());
  }

  std::vector<GattSubscription> subscriptions;
  subscriptions.push_back(std::move(subscribed).Value());
  co_return BluetoothResult<std::vector<GattSubscription>>::Success(std::move(subscriptions));
}
```

The application event handler starts the same connection operation with the retained device. It checks its auto-reconnect setting, the error, and its current lifecycle before scheduling a Task. A service change on a live session uses the existing discovery and subscription setup directly.

```cpp
void OnGattEvent(GattEvent event) {
  const auto* disconnected = std::get_if<GattDisconnected>(&event);
  if (!disconnected) {
    RefreshCurrentGattSubscriptions();
    return;
  }

  ClearCurrentConnection();
  if (disconnected->error && ShouldReconnect(*disconnected->error)) {
    StartConnectionTaskForSelectedDevice();
  }
}
```

The Preview stores one selected device identifier in the application data directory. At startup it tries to restore the selection using existing access. The Restore saved device action lets the user retry and request permission where needed. Connect opens the selected device on demand. The Auto reconnect switch applies to the current application run. After an eligible unexpected disconnect while active, the Preview opens a fresh session and restores the previous subscription by its service and characteristic UUIDs. Cancel connection and turning off Auto reconnect both cancel pending recovery. Forget saved device clears the Preview's saved selection.

## Extension contracts

### Advertising and local GATT

Add both operations to the existing `Bluetooth` service. `can_advertise` reports whether the current backend and adapter support any advertising path. `can_use_gatt_server` reports whether they can open a local GATT server. The application owns advertising and local service configuration through separate move-only handles.

Use `BluetoothCapabilities::can_advertise_standalone` to offer standalone advertising in the application UI. A true `can_advertise_standalone` implies `can_advertise`. An application can offer service-associated advertising when both `can_advertise` and `can_use_gatt_server` are true. The operation result accounts for the requested combination, payload, permissions, and resources.

```cpp
/// Supports at least one advertising mode without an open local GATT server.
bool can_advertise_standalone = false;
```

#### Public declarations

The declarations in `huxerui::bluetooth` use the same `Task`, `BluetoothResult`, `OperationOptions`, `Bytes`, `GattCharacteristicProperties`, and UUID formats as the client. Private handle storage is omitted.

```cpp
/// @brief Normal termination of one advertising or server session.
enum class PeripheralEndReason { Stopped, Backgrounded };

/// @brief A normal stop or a terminal error reported by the session.
using PeripheralEndEvent = std::variant<PeripheralEndReason, BluetoothError>;

/// @brief Rejection to send for an incoming attribute request.
enum class GattRequestError { NotPermitted, InvalidValueLength, UnlikelyError };

/// @brief Definition and request handlers for one local characteristic.
struct LocalGattCharacteristic {
  /// UUID within the enclosing service.
  std::string uuid;

  /// Properties to expose. The first release supports read, write with response, and notify.
  GattCharacteristicProperties properties;

  /// Returns the complete current value for one ATT read request, including a read at a nonzero offset.
  std::function<Result<Bytes, GattRequestError>()> on_read;

  /// Accepts or rejects one complete replacement value from a remote write.
  std::function<Result<void, GattRequestError>(Bytes)> on_write;

  /// Reports whether the characteristic has any subscribed peers.
  std::function<void(bool has_subscribers)> on_subscription_changed;
};

/// @brief Definition of one primary service on the local GATT server.
struct LocalGattService {
  /// UUID unique within the enclosing server.
  std::string uuid;

  /// Characteristics to publish in this service.
  std::vector<LocalGattCharacteristic> characteristics;
};

/// @brief Unique owner of a set of local service definitions.
class GattServerSession final {
public:
  GattServerSession(GattServerSession&& other) noexcept;

  GattServerSession& operator=(GattServerSession&& other) noexcept;

  ~GattServerSession();

  /// @brief Submits a value update to the characteristic's subscribed peers.
  /// @param service_uuid UUID of a service configured by this session.
  /// @param characteristic_uuid UUID of the notification characteristic within the service.
  /// @param value Owned update bytes.
  /// @param options Deadline for accepting and submitting the update.
  /// @return Success when the platform accepts the update, or an operation error.
  Task<BluetoothResult<void>> NotifyAsync(std::string service_uuid, std::string characteristic_uuid,
                                          Bytes value, OperationOptions options = {});

  /// @brief Retires the local services. Retain the handle to receive the terminal event.
  void Stop() noexcept;
};

struct AdvertisingOptions {
  /// Service UUIDs to advertise. Accepts the same UUID formats as ScanOptions.
  std::vector<std::string> service_uuids;

  /// Requested local name, subject to the platform's advertising rules.
  std::optional<std::string> local_name;

  /// Requested connectability. An empty value uses the default for the selected overload.
  std::optional<bool> connectable;
};

/// @brief Unique owner of one advertising operation.
class AdvertisingSession final {
public:
  AdvertisingSession(AdvertisingSession&& other) noexcept;

  AdvertisingSession& operator=(AdvertisingSession&& other) noexcept;

  ~AdvertisingSession();

  /// @brief Stops this advertisement. Retain the handle to receive the terminal event.
  void Stop() noexcept;
};

// Members of Bluetooth.
/// @brief Opens a local GATT server with the supplied primary services and handlers.
/// @param services Owned service definitions and callbacks.
/// @param options Deadline for opening the server.
/// @param on_end Optional terminal callback for an acquired server session.
/// @return A server with its configuration ready, or an operation error. Publication follows platform rules.
Task<BluetoothResult<GattServerSession>> OpenGattServerAsync(
    std::vector<LocalGattService> services, OperationOptions options = {},
    std::function<void(PeripheralEndEvent)> on_end = {});

/// @brief Starts a standalone advertisement.
/// @param advertisement Service UUIDs, name, and connectability requested for the advertisement.
/// @param options Deadline for startup.
/// @param on_end Optional terminal callback for an acquired advertising session.
/// @return An active advertising session or an operation error.
Task<BluetoothResult<AdvertisingSession>> StartAdvertisingAsync(
    AdvertisingOptions advertisement, OperationOptions options = {},
    std::function<void(PeripheralEndEvent)> on_end = {});

/// @brief Starts advertising services configured by a local GATT server.
/// @param server Server that owns the selected services.
/// @param advertisement The local service UUIDs to advertise and any supported payload options.
/// @param options Deadline for startup.
/// @param on_end Optional terminal callback for an acquired advertising session.
/// @return An active advertising session or an operation error.
Task<BluetoothResult<AdvertisingSession>> StartAdvertisingAsync(
    const GattServerSession& server, AdvertisingOptions advertisement, OperationOptions options = {},
    std::function<void(PeripheralEndEvent)> on_end = {});
```

#### Advertising and ownership

- The first advertising overload requests a standalone advertisement. Supply service UUIDs, a nonempty local name, or both. An empty `connectable` uses the platform default. An explicit value requests that mode.
- The server overload requests connectable advertising for a nonempty subset of that server's service UUIDs. `service_uuids` selects which UUIDs to advertise. The complete server configuration defines the set of services available for remote discovery once published. On platforms that publish through advertising, successful startup covers the complete configured set. A platform that cannot satisfy the requested combination returns `Unsupported`. An explicit `false` is invalid for this overload. The call captures the server's identity before the Task starts. The server must still be open on the same application and adapter at admission. Retaining the advertising handle never extends server lifetime.
- Complete startup when the platform confirms advertising has started. Report `Unsupported` for an unavailable advertising mode or payload field, `PayloadTooLarge` for a known size violation, and `Busy` for resource contention. Success reflects platform acceptance. Packet contents and discovery remain subject to platform policy. When the platform explicitly reports that requested data was omitted, fail startup and roll back the advertisement.
- Each start is a lazy Task with the existing per-operation deadline. Admit new sessions while the application is active. Inactive transitions let existing sessions and pending starts continue. Entering the background retires both features and returns `Canceled` to callers still awaiting a start. Canceling the caller's Task detaches its continuation. An acquired session gets at most one terminal event. `Stop()` is idempotent and queues `Stopped`. A background transition queues `Backgrounded`. Use and destroy handles on the owning application thread. Destruction retires the session and suppresses pending delivery.
- Stopping advertising leaves the logical server and its definitions available for another advertising start. Stopping a server ends advertising explicitly associated with it. A standalone advertisement keeps its own lifetime. On platforms where advertising controls publication, stopping associated advertising can withdraw remote access to the services while retaining the logical server.
- A failed or canceled start rolls back partial platform registration. Keep the affected resource slot until late platform completion and cleanup are isolated from the next start. Startup errors go to the Task. Terminal events apply to sessions already handed to the caller.

#### Local service registration and requests

`OpenGattServerAsync()` takes ownership of the definitions and handlers. Success means the full configuration is ready. Backends that register local services independently publish them as part of opening the server. On Windows, associated advertising publishes the configured service. Each primary service requires a unique UUID within the server and each characteristic requires a unique UUID within its service. Reject empty definitions and duplicates with `InvalidArgument`. Select a notification characteristic by its service UUID and characteristic UUID. `GattService` and `GattCharacteristic` continue to represent remote discovery results.

Activate application handlers only after server startup has committed. Until then, reject peer requests through the platform request path. An adapter can limit concurrent server registrations, advertising sessions, or combinations with scanning and client connections. Report `Busy` when an existing operation occupies a required resource.

The initial contract supports read, write with response, and notify. Readable characteristics require `on_read` and writable characteristics require `on_write`. `on_subscription_changed` is optional for notification characteristics. Return `UnsupportedProperty` for other requested properties and `InvalidArgument` for missing required handlers. The platform or backend manages the Client Characteristic Configuration Descriptor (CCCD). Manufacturer and service data, custom descriptors, configurable attribute protection, indications, peer-targeted updates, and additional write modes can extend the respective operations later. Use the initial service contract for values that can be exposed without application-configured link protection.

The backend owns each platform request and responds once within its deadline. Handlers run synchronously on the owning application thread and must return promptly. Map `GattRequestError` to the supported ATT rejection. A throwing request handler produces `UnlikelyError` where a response is possible. Application operation errors still use `BluetoothError`.

`on_read()` returns a complete owned value for one ATT request. A follow-up Read Blob request can invoke the handler again and observe a newer value. The backend applies the offset and response limit to that request's value, rejecting an offset beyond its end. Applications that require a consistent long value must keep the value stable across requests or provide consistency through their device protocol. [Bluetooth ATT long attribute reads](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core_v7.0/out/en/host/attributeprotocol-att--specification.html)

`on_write()` receives one complete replacement at offset zero. Reject write commands, prepared writes, nonzero offsets, and grouped transactions when exposed by the platform, before invoking the handler. If the platform supplies a single completed replacement, pass that replacement to the handler.

Subscription callbacks report changes in whether at least one peer is subscribed, starting from an empty set. Use per-peer membership when visible to the backend and characteristic-level state on aggregate notification paths. Retire affected subscriptions on disconnect or unpublication when observable. `NotifyAsync()` submits to the then-subscribed peers of a published characteristic and succeeds as a no-op if there are none. Return `Unavailable` for a configured service that the platform has yet to publish. Success reflects platform submission. A failure can occur after partial submission. Enforce known payload limits and bounded flow control. Retire pending updates when the server closes. The application owns the value returned by `on_read` and decides whether a failed update can be retried.

#### Platform mapping

| Target | Advertising | Local GATT server |
| --- | --- | --- |
| Android | Use `BluetoothLeAdvertiser` and await the start callback. A requested local name is supported only when the platform can include the matching adapter name. | Use `BluetoothGattServer`. Register services sequentially and wait for each callback. Track CCCD subscriptions per peer and use the notification callback for flow control. [Advertiser](https://developer.android.com/reference/android/bluetooth/le/BluetoothLeAdvertiser) · [GATT server](https://developer.android.com/reference/android/bluetooth/BluetoothGattServer) |
| iOS | Use `CBPeripheralManager` for service UUIDs and the optional name. Reject an explicit mode when Core Bluetooth cannot guarantee the requested connectability. | Publish through the iOS peripheral manager delegate. Map read offsets, write batches, and send-queue readiness. [Core Bluetooth](https://developer.apple.com/documentation/corebluetooth/cbperipheralmanager) · [Peripheral role tasks](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/PerformingCommonPeripheralRoleTasks/PerformingCommonPeripheralRoleTasks.html) |
| macOS | Apply the same Core Bluetooth payload limits. The application supplies its Bluetooth usage description and sandbox entitlement where applicable. | Implement the peripheral manager delegate and lifetime in the macOS backend. [Core Bluetooth advertising](https://developer.apple.com/documentation/corebluetooth/cbperipheralmanager/startadvertising(_:)) · [Bluetooth entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.device.bluetooth) |
| Windows | Start service-associated advertising through `GattServiceProvider`. The generic publisher reserves UUID and name fields, so set `can_advertise_standalone = false` and return `Unsupported` for standalone advertising in this release. | The initial backend supports one primary service per server. `OpenGattServerAsync()` configures its provider and characteristics and returns `Unsupported` for a valid multi-service request. Associated advertising publishes the configured service. Stopping it can withdraw remote access while preserving the configuration. Gate the features on WinRT, the adapter, and app configuration. Windows 7 reports both features as unsupported. [GATT server](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-server#publish-the-service) · [Publisher restrictions](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisher#remarks) |
| Linux | Register an advertising object with BlueZ `LEAdvertisingManager1` when available on the adapter. | Register a separate local service tree through `GattManager1`. Map `StartNotify()` and `StopNotify()` to aggregate subscription state. [Advertising API](https://bluez.readthedocs.io/en/latest/advertising-api/) · [GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/) |
| Web | Report both advertising flags as false and return `Unsupported`. | Report `can_use_gatt_server = false` and return `Unsupported`. Browser GATT access uses the existing client API. [Web Bluetooth guide](https://developer.chrome.com/docs/capabilities/bluetooth) |

Applications supply declarations and request permissions. Android 12 and later can require `BLUETOOTH_ADVERTISE` for advertising and `BLUETOOTH_CONNECT` for the server, depending on the target SDK. The Android Preview requests advertising access through its Activity and uses `Permission::BluetoothConnect` for the server. A missing grant returns `PermissionDenied`. If a submitted Android notification is interrupted, the backend retires its server to isolate the outstanding platform callback. iOS and macOS have separate platform implementations. [Android Bluetooth permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions)

#### Application example

Open the local server and then advertise its service on a supported host. Keep both handles in the owning model. The characteristic holds a one-byte value that fits in a single ATT read response.

```cpp
constexpr char service_uuid[] = "e1120001-14f2-43f3-a98b-cfd9b47fd9bb";
constexpr char value_uuid[] = "e1120002-14f2-43f3-a98b-cfd9b47fd9bb";

struct PeripheralModel {
  std::shared_ptr<Bytes> value = std::make_shared<Bytes>(1, std::byte{0});
  std::optional<GattServerSession> server;
  std::optional<AdvertisingSession> advertising;
};

Task<void> OpenPeripheral(std::shared_ptr<Bluetooth> bluetooth, std::shared_ptr<PeripheralModel> model) {
  LocalGattCharacteristic characteristic{
      .uuid = value_uuid,
      .properties = {.can_read = true, .can_write_with_response = true, .can_notify = true},
      .on_read = [value = model->value] {
        return Result<Bytes, GattRequestError>::Success(*value);
      },
      .on_write = [value = model->value](Bytes replacement) {
        if (replacement.size() != 1)
          return Result<void, GattRequestError>::Failure(GattRequestError::InvalidValueLength);
        *value = std::move(replacement);
        return Result<void, GattRequestError>::Success();
      },
  };
  std::vector<LocalGattService> services{{service_uuid, {std::move(characteristic)}}};
  auto server = co_await bluetooth->OpenGattServerAsync(std::move(services));
  if (!server.Succeeded()) {
    ShowBluetoothError(server.Error());
    co_return;
  }
  model->server.emplace(std::move(server).Value());

  AdvertisingOptions advertisement{.service_uuids = {service_uuid}};
  auto advertising = co_await bluetooth->StartAdvertisingAsync(*model->server, std::move(advertisement));
  if (advertising.Succeeded()) {
    model->advertising.emplace(std::move(advertising).Value());
  } else {
    ShowBluetoothError(advertising.Error());
  }
}
```

When the application changes the value, call `co_await model->server->NotifyAsync(service_uuid, value_uuid, *model->value)` and handle the result. The Preview exposes controls to open the server, advertise its service, and notify subscribed peers. Each handle has its own stop action. The existing client can scan for `service_uuid`, then discover, read, write, and subscribe through its `GattSession`.

### RFCOMM and LE L2CAP

**Status** — Implemented API

Open RFCOMM by Classic service UUID and LE L2CAP by the peer's Protocol/Service Multiplexer (PSM). Both operations return a move-only `BluetoothChannel` with the same byte-stream contract. Reuse `Device`, `Bytes`, `Task`, `BluetoothResult`, `OperationOptions`, and `TaskHandle` cancellation. Read and write follow the SDK's `AsyncInputStream` and `AsyncOutputStream` conventions for bounded reads, EOF, and complete writes. The channel owns both directions and retains Bluetooth errors and operation deadlines.

The API covers outbound channels. Classic discovery starts with system-paired candidates. A device protocol can use the existing BLE scan, device recovery, and GATT APIs to find its LE peer and obtain the endpoint.

#### Public declarations

The channel operations add the following fields to `BluetoothCapabilities`. Each field reports an independently available operation on the current backend and adapter. Operation results account for peer support, access, adapter state, and resource limits.

```cpp
/// Supports listing system-paired Classic candidates for RFCOMM.
bool can_list_paired_rfcomm_devices = false;

/// Supports opening a Classic RFCOMM channel by service UUID.
bool can_connect_rfcomm = false;

/// Supports opening an LE L2CAP credit-based channel by PSM.
bool can_connect_le_l2cap = false;
```

```cpp
/// @brief Terminal event for an acquired Bluetooth channel.
/// At orderly peer EOF, delivery waits until all buffered bytes have reached a reader.
struct BluetoothChannelClosed {
  /// Empty after an explicit close or observed orderly peer EOF.
  std::optional<BluetoothError> error;
};

/// @brief Unique owner of a foreground Bluetooth byte stream.
/// Use and destroy the channel on the owning application thread.
class BluetoothChannel final {
public:
  BluetoothChannel(BluetoothChannel&& other) noexcept;

  BluetoothChannel& operator=(BluetoothChannel&& other) noexcept;

  ~BluetoothChannel();

  /// @brief Reads the next available chunk in stream order.
  /// @param maximum_bytes Positive upper bound for the returned chunk.
  /// @param options Deadline for the read.
  /// @return Owned bytes, an empty success after buffered data drains at orderly EOF, or an operation error.
  Task<BluetoothResult<Bytes>> ReadAsync(std::size_t maximum_bytes, OperationOptions options = {});

  /// @brief Submits the supplied bytes in stream order.
  /// @param value Owned bytes retained for the duration of the operation.
  /// @param options Deadline for accepting the complete value.
  /// @return Success after platform acceptance of all bytes, or an operation error.
  Task<BluetoothResult<void>> WriteAsync(Bytes value, OperationOptions options = {});

  /// @brief Stops both directions and retires pending I/O.
  /// Close is idempotent and discards unread bytes. Retain the handle to receive the terminal event.
  void Close() noexcept;
};

// Members of Bluetooth.
/// @brief Lists paired devices that are candidates for Classic RFCOMM.
/// @param options Deadline for reading the system's paired-device records.
/// @return Backend-scoped devices, possibly an empty list, or an operation error.
Task<BluetoothResult<std::vector<Device>>> ListPairedRfcommDevicesAsync(OperationOptions options = {});

/// @brief Resolves a Classic service and opens its RFCOMM channel.
/// @param device Device with a Classic identity from this application backend.
/// @param service_uuid Service class UUID in the same formats as ScanOptions.
/// @param options Deadline covering service resolution and channel open.
/// @param on_closed Optional terminal callback for the acquired channel.
/// @return An open channel or an operation error. A conflicting open can return Busy during platform cleanup.
Task<BluetoothResult<BluetoothChannel>> ConnectRfcommAsync(
    Device device, std::string service_uuid, OperationOptions options = {},
    std::function<void(BluetoothChannelClosed)> on_closed = {});

/// @brief Opens an LE L2CAP credit-based channel.
/// @param device LE peer recognized by this application backend.
/// @param psm Current endpoint from the peer's device protocol.
/// @param options Deadline covering link acquisition and channel open.
/// @param on_closed Optional terminal callback for the acquired channel.
/// @return An open channel or an operation error. A conflicting open can return Busy during platform cleanup.
Task<BluetoothResult<BluetoothChannel>> ConnectLeL2capAsync(
    Device device, std::uint16_t psm, OperationOptions options = {},
    std::function<void(BluetoothChannelClosed)> on_closed = {});
```

#### Device and endpoint resolution

`ListPairedRfcommDevicesAsync()` reads system-paired Classic candidates from the selected adapter where the platform exposes adapter scope. It skips known LE-only devices and can include candidates whose transport is unknown. The list only describes system records. Resolve the service on the selected peer during `ConnectRfcommAsync()`, using the platform SDP path. Where the backend finds multiple distinct matches without a platform selection, return `Unsupported` for that request. A valid device or endpoint that is no longer available returns `DeviceUnavailable`.

Keep transport-specific identity data inside `Device`. Classic and LE identities can differ for the same physical peer. Extend `RestoreDeviceAsync()` to look up saved Classic identifiers through the backend's paired-device records. RFCOMM preferences store `Device::Id()` and the service UUID. Malformed UUIDs, out-of-range PSMs, and devices from a different application backend return `InvalidArgument`.

LE credit-based connections use the LE SPSM range `0x0001` through `0x00FF`. The portable application range is `0x0080` through `0x00FF`. A valid fixed PSM can return `Unsupported` where the platform only exposes dynamic endpoints. Read a dynamic PSM from the peer's service before each connection, including reconnects. Use `GattSession::ReadAsync()` when the device profile publishes the PSM as a characteristic. The L2CAP open takes a `Device` and acquires or reuses the LE link. GATT attribute handles stay scoped to their discovery snapshot. [Bluetooth Core L2CAP specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/host/logical-link-control-and-adaptation-protocol-specification.html)

Applications request the applicable access before connecting. The platform can require pairing or consent during service access. Use the protected connection variant where the platform provides that choice. Preserve authorization and security failures through existing Bluetooth errors. Protection levels follow the platform and peer.

#### Channel I/O and lifetime

| Operation | Contract |
| --- | --- |
| `ReadAsync(maximum_bytes)` | Wait for available data, then return up to the requested size. Empty success reports orderly EOF. A zero size returns `InvalidArgument`. Applications frame messages on top of the stream. |
| `WriteAsync(value)` | Success means the platform accepted every byte in order. An empty value on an open channel succeeds without sending data. A failure may leave a written prefix. The device protocol defines acknowledgement and retry. |
| `Close()` | Retire the logical channel immediately, discard unread data, and start platform cleanup. Pending operations on a live caller receive `Canceled`. Later I/O returns `Disconnected`. Use application acknowledgements when delivery must be confirmed before closing. |

Tasks are lazy. Start the deadline and reserve the operation slot when the Task runs. Admit one read and one write concurrently per channel. A second operation in the same direction returns `Busy`. Preserve byte order through partial platform reads and writes. For an LE L2CAP backend using packet sockets, receive each SDU in full and retain bytes beyond the caller's read size. Segment writes to the platform's transmit limits. Bound buffering and apply platform flow control. If a push-only transport exhausts the receive buffer, close with a diagnostic `PlatformFailure`.

Cancellation through `TaskHandle` or its scope detaches the caller's continuation. A deadline returns `TimedOut` to a live caller. A canceled or timed-out read consumes no bytes before handoff. Keep one platform receive path and retain bytes until a read hands them to a live caller. An interrupted write can leave the channel usable when the backend can establish that zero bytes were submitted. Otherwise retire the channel to protect byte-stream integrity.

On an observable orderly peer EOF, fail new writes and any write still awaiting platform acceptance with `Disconnected`. A platform can report the output stream ending before input EOF. In that case, reads continue after writes end. Reads return buffered bytes and then return empty success at input EOF. Queue `on_closed` after the last buffered bytes have reached a live reader, or immediately if no bytes are buffered. The callback can therefore remain pending while the application has unread bytes. An undifferentiated link loss reports `Disconnected`. Explicit close and destruction retire both directions immediately and discard unread bytes. Destruction suppresses pending terminal delivery. An acquired channel reports at most one terminal event on the owning application thread.

Start listing and connecting while the application is active. Inactive transitions allow admitted work to continue. Entering the background cancels pending opens and retires acquired channels with `Canceled`. Adapter or permission loss retires affected channels using the reported Bluetooth error.

Canceling or timing out an open retires the caller's attempt and starts platform cleanup. Close any channel returned by a late completion for that attempt. A conflicting open returns `Busy` until the previous open completes, its cancellation is confirmed, or the backend can safely distinguish its callbacks from the new attempt. Keep an active GATT session connected while resolving an outstanding L2CAP open.

The backend tracks physical link use per peer. Each GATT session and LE L2CAP channel holds a separate claim. A pending open holds its claim while it needs the link. Closing a GATT session or L2CAP channel releases its own operations, handles, and claim. If the backend established the link, it can disconnect it after the last claim is released and any other affected transport is done. Release a borrowed link without requesting a device-wide disconnect. Actual link loss is reported to all affected owners. For example, an application can read the PSM over GATT, open the L2CAP channel, and then disconnect the GATT session while continuing L2CAP I/O.

#### Platform mapping

| Target | RFCOMM | LE L2CAP |
| --- | --- | --- |
| Android | List candidates with `getBondedDevices()`, create the RFCOMM socket with `createRfcommSocketToServiceRecord()`, and connect on a worker. Keep the socket to interrupt a pending blocking connect or write. | Use `createL2capChannel()` on API 29 and later for dynamic PSMs. Reuse the `BluetoothSocket` stream path. The app requests `BLUETOOTH_CONNECT` where required. [Android device sockets](https://developer.android.com/reference/android/bluetooth/BluetoothDevice#createL2capChannel(int)) · [Android socket lifetime](https://developer.android.com/reference/android/bluetooth/BluetoothSocket) |
| iOS | Report `Unsupported` for generic RFCOMM. Core Bluetooth Classic exposes a GATT path. External Accessory uses an accessory protocol. | Open with `CBPeripheral::openL2CAPChannel` and use `CBL2CAPChannel` streams. Share the Core Bluetooth peer and delegate with GATT and keep independent logical lifetimes. [Core Bluetooth Classic](https://developer.apple.com/videos/play/wwdc2019/901/) · [L2CAP streams](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel) |
| macOS | Use `IOBluetoothDevice` paired-device records, an SDP query, and `openRFCOMMChannelAsync`. Honor the sandbox Bluetooth entitlement where applicable. | Implement LE link acquisition and `CBL2CAPChannel` in the macOS backend separately from iOS. [IOBluetooth device API](https://developer.apple.com/documentation/iobluetooth/iobluetoothdevice) · [Core Bluetooth L2CAP](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel) |
| Windows | Enumerate paired Classic devices and resolve the service with WinRT RFCOMM. Open a `StreamSocket` for the resolved service and honor platform consent and packaging requirements. | Report `Unsupported` for LE L2CAP in this backend. The Windows 7 compatibility build reports both transports as unsupported. [Windows RFCOMM](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/send-or-receive-files-with-rfcomm) · [WinRT service resolution](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.rfcomm.rfcommdeviceservice) |
| Linux | Use paired BlueZ device records and a client profile for the UUID. Open only after receiving the matching socket through `Profile1.NewConnection`. A profile registration conflict returns `Busy`. | Use an `AF_BLUETOOTH` LE L2CAP socket on the selected adapter and the peer's known address type. Bridge complete SDUs to the shared stream using negotiated socket limits. [BlueZ profile API](https://bluez.readthedocs.io/en/latest/profile-api/) · [BlueZ socket and MTU handling](https://github.com/bluez/bluez/blob/master/tools/l2cap-tester.c) |
| Web | The Web Bluetooth backend reports `Unsupported`. Chromium Web Serial can access certain RFCOMM services through its own port chooser and grant. | The Web Bluetooth backend reports `Unsupported` for LE L2CAP. [Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth) · [Chromium Web Serial over Bluetooth](https://developer.chrome.com/blog/serial-over-bluetooth) |

Build the Linux backend with the BlueZ development headers. The macOS RFCOMM backend admits one active channel or pending open per Classic peer to preserve exclusive delegate ownership.

Serialize pending Core Bluetooth channel opens per peripheral. A failed delegate callback can omit the channel object. After cancellation or timeout, the slot remains occupied until the delegate completion arrives or link or manager teardown makes callbacks from the previous open safe to ignore. On Linux, disable profile auto-connect and use `Device1.ConnectProfile()` for the selected UUID. Correlate the resulting file descriptor by client profile and device path, and close unmatched descriptors.

#### Application example

Both connect operations use the same I/O function. This example writes an application payload and consumes one available chunk. An application parser can keep reading until it has a complete message.

```cpp
Task<void> ExchangeChunk(BluetoothChannel channel, Bytes payload) {
  auto written = co_await channel.WriteAsync(std::move(payload));
  if (!written.Succeeded()) {
    ShowBluetoothError(written.Error());
    co_return;
  }

  auto received = co_await channel.ReadAsync(1024);
  if (!received.Succeeded()) {
    ShowBluetoothError(received.Error());
  } else if (!received.Value().empty()) {
    ConsumeChunk(std::move(received).Value());
  }
  channel.Close();
}

Task<void> OpenRfcomm(Bluetooth& bluetooth, Device selected, std::string service_uuid, Bytes payload) {
  auto opened = co_await bluetooth.ConnectRfcommAsync(std::move(selected), std::move(service_uuid));
  if (opened.Succeeded()) {
    co_await ExchangeChunk(std::move(opened).Value(), std::move(payload));
  } else {
    ShowBluetoothError(opened.Error());
  }
}

Task<void> OpenLeChannel(Bluetooth& bluetooth, Device peer, std::uint16_t current_psm, Bytes payload) {
  auto opened = co_await bluetooth.ConnectLeL2capAsync(std::move(peer), current_psm);
  if (opened.Succeeded()) {
    co_await ExchangeChunk(std::move(opened).Value(), std::move(payload));
  } else {
    ShowBluetoothError(opened.Error());
  }
}
```

### Other transports

Apple External Accessory uses the accessory protocol declared by the application. Device profiles such as audio, keyboard input, and MIDI belong to the relevant OS services or application libraries. [External Accessory](https://developer.apple.com/documentation/externalaccessory/)

### Background communication and process restoration

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
