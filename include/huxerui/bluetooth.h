#pragma once

/// @file
/// @brief Foreground Bluetooth LE discovery, pairing, and GATT client operations for HuxerUI.
///
/// Link HuxerUI::Bluetooth and register bluetooth::Install through the application hooks. Use the returned session
/// handles to control operation lifetimes. Applications supply their permission declarations and request access.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/task.h>

namespace huxerui::bluetooth {

namespace detail {
struct Access;
struct Attribute;
struct DeviceData;
struct Scan;
struct Service;
struct Session;
struct Subscription;
} // namespace detail

/// @brief Stable failure categories for application decisions.
enum class BluetoothErrorCode {
  /// The backend or host does not implement the requested operation.
  Unsupported,
  /// A required adapter, service, or application configuration is unavailable.
  Unavailable,
  /// The platform reports that the Bluetooth adapter is powered off.
  AdapterOff,
  /// The application or device grant does not allow the operation.
  PermissionDenied,
  /// The remote attribute requires authentication, authorization, or encryption.
  AuthenticationRequired,
  /// The attempt to authenticate or establish a bond failed.
  AuthenticationFailed,
  /// The operation was canceled and reported the outcome to a live caller.
  Canceled,
  /// The application deadline elapsed or the platform reported a timeout.
  TimedOut,
  /// An active operation or a full request queue prevents accepting the request.
  Busy,
  /// The backend does not recognize the device or cannot currently reach it.
  DeviceUnavailable,
  /// The logical GATT session is closed or the connection was lost.
  Disconnected,
  /// The attribute belongs to a different session or a retired discovery snapshot.
  StaleAttribute,
  /// The operation arguments violate the API contract.
  InvalidArgument,
  /// The attribute or backend does not allow the requested access or write mode.
  UnsupportedProperty,
  /// The payload exceeds the known write limit or the peer rejected its length.
  PayloadTooLarge,
  /// The platform reported a failure without a more specific portable category.
  PlatformFailure,
};

/// @brief A failed Bluetooth operation with diagnostic information from the platform when available.
struct BluetoothError {
  /// Stable category for programmatic error handling.
  BluetoothErrorCode code;

  /// Human-readable diagnostic. Its text can vary by platform and locale.
  std::string message;

  /// Platform diagnostic namespace or identifier. Empty when unavailable.
  std::string platform_domain;

  /// Numeric status within the platform domain when available.
  std::optional<std::int64_t> platform_code;
};

/// @brief An owned Bluetooth operation result.
///
/// Check Succeeded() before accessing Value() or Error(). Copy or move the stored value to retain it independently.
/// @tparam T Success payload. Use void for an operation that reports completion without a payload.
template <class T> using BluetoothResult = Result<T, BluetoothError>;

/// @brief Deadline for a queued or executing asynchronous operation, measured from Task start.
struct OperationOptions {
  /// Maximum time for queueing and execution. The timeout must be positive.
  /// System interaction already in progress can outlive this deadline. If an active GATT request reaches the
  /// deadline, the library closes the logical session. Connect again before issuing further GATT requests.
  std::chrono::milliseconds timeout = std::chrono::seconds{30};
};

/// @brief Availability and power information observable through the current platform backend.
enum class AdapterStatus {
  /// The backend has not established the current adapter status, or the platform does not expose it.
  Unknown,
  /// The backend reports that the adapter or a required platform service is unavailable.
  Unavailable,
  /// The platform reports that the radio is off.
  PoweredOff,
  /// The platform reports that the radio is on. Access and device reachability are checked separately.
  PoweredOn,
};

/// @brief System bond information when exposed by the platform.
enum class BondState {
  /// The backend cannot establish the current bond state.
  Unknown,
  /// The system reports no saved bond.
  NotBonded,
  /// The system reports that bonding is in progress.
  Bonding,
  /// The system reports an established bond. This state is independent of the connection.
  Bonded,
};

/// @brief Operations the current backend and host support. Permission and device readiness are checked at call time.
struct BluetoothCapabilities {
  /// Supports foreground advertisement scanning through StartScan().
  bool can_scan = false;
  /// Supports a system device chooser through SelectDevice().
  bool can_select_device = false;
  /// Supports attempts to resolve saved identifiers through RestoreDeviceAsync().
  bool can_restore_device = false;
  /// Supports explicit pairing through PairAsync().
  bool can_pair = false;
  /// Supports querying the system bond through GetBondStateAsync().
  bool can_query_bond = false;
  /// Supports opening a GATT client session through ConnectAsync().
  bool can_use_gatt_client = false;
  /// Supports descriptor access when the discovered attribute and its permissions allow it.
  bool can_access_descriptors = false;
  /// Reports support for BLE advertising. Always false in this release.
  bool can_advertise = false;
  /// Reports support for a local GATT server. Always false in this release.
  bool can_use_gatt_server = false;
};

/// @brief A discovered, selected, or restored device owned by the original application backend.
class Device final {
public:
  /// @brief Returns the opaque device identifier. The reference is valid while this value retains its device data.
  /// Save a copy as a recovery hint when the backend supports RestoreDeviceAsync().
  const std::string& Id() const;

  /// @brief Returns the device display name when available. The reference borrows this value's device data.
  const std::optional<std::string>& Name() const;

  /// @brief Compares the original backend and its device identity.
  bool operator==(const Device& other) const noexcept;

private:
  explicit Device(std::shared_ptr<const detail::DeviceData> data);

  std::shared_ptr<const detail::DeviceData> data_;
  friend struct detail::Access;
};

/// @brief Options for one bounded foreground scan.
struct ScanOptions {
  /// A device matches when it advertises any listed service. Empty accepts all advertisements.
  /// Specify each UUID as 4 or 8 hexadecimal digits or a hyphenated 128-bit UUID.
  std::vector<std::string> advertised_services;

  /// Maximum scan duration. Must be positive.
  std::chrono::milliseconds duration = std::chrono::seconds{10};
};

/// @brief Options for a platform-owned device chooser.
/// Service UUIDs use the same input formats as ScanOptions::advertised_services.
struct SelectDeviceOptions {
  /// Advertised services eligible for selection. A device can match any listed service.
  std::vector<std::string> advertised_services;

  /// Additional GATT services the application requests access to after selection.
  std::vector<std::string> optional_services;

  /// Enables an unfiltered chooser. An empty advertised_services list requires this flag, and a nonempty list
  /// requires it to be false.
  bool accept_all_devices = false;
};

/// @brief Advertising data observed during discovery. Unknown scalar observations are empty.
/// Advertised UUIDs and service data keys use the lowercase, hyphenated 128-bit form.
struct Advertisement {
  /// Name advertised in this observation when available.
  std::optional<std::string> local_name;
  /// Service UUIDs present in the observation.
  std::vector<std::string> service_uuids;
  /// Manufacturer payload bytes keyed by Bluetooth company identifier.
  std::map<std::uint16_t, Bytes> manufacturer_data;
  /// Service payload bytes keyed by service UUID.
  std::map<std::string, Bytes> service_data;
  /// Received signal strength in dBm when reported by the platform.
  std::optional<int> rssi_dbm;
  /// Whether the advertisement was connectable when reported by the platform.
  std::optional<bool> connectable;
};

/// @brief A device update reported by the active scan. A device can produce multiple observations.
struct ScanObservation {
  Device device;
  Advertisement advertisement;
};

/// @brief Normal terminal outcomes for a foreground scan.
enum class ScanEndReason { DurationElapsed, Stopped };

/// @brief An observation, normal termination, or terminal failure.
using ScanEvent = std::variant<ScanObservation, ScanEndReason, BluetoothError>;

/// @brief Unique owner of a foreground scan. Use and destroy the owner on the application thread.
/// Moving transfers ownership and destruction stops the scan and suppresses pending delivery.
class ScanSession final {
public:
  ScanSession(ScanSession&& other) noexcept;

  ScanSession& operator=(ScanSession&& other) noexcept;

  ~ScanSession();

  /// @brief Requests an idempotent stop. Retain the handle until the queued Stopped event is delivered.
  void Stop() noexcept;

private:
  explicit ScanSession(std::shared_ptr<detail::Scan> scan);

  std::shared_ptr<detail::Scan> scan_;
  friend struct detail::Access;
};

/// @brief Selects whether the GATT operation asks the peer for a write response.
enum class GattWriteMode { WithResponse, WithoutResponse };

/// @brief Properties declared by a GATT characteristic. Security and platform policy are checked during use.
struct GattCharacteristicProperties {
  bool can_read = false;
  bool can_write_with_response = false;
  bool can_write_without_response = false;
  bool can_notify = false;
  bool can_indicate = false;
};

/// @brief A descriptor instance belonging to one discovery snapshot of one GATT session.
class GattDescriptor final {
public:
  /// @brief Returns the lowercase, hyphenated 128-bit descriptor UUID.
  /// Its metadata remains available after invalidation.
  const std::string& Uuid() const;

  /// @brief Compares descriptor instance identity within the originating snapshot.
  bool operator==(const GattDescriptor& other) const noexcept;

private:
  explicit GattDescriptor(std::shared_ptr<const detail::Attribute> attribute);

  std::shared_ptr<const detail::Attribute> attribute_;
  friend struct detail::Access;
};

/// @brief A characteristic instance belonging to one discovery snapshot of one GATT session.
class GattCharacteristic final {
public:
  /// @brief Returns the lowercase, hyphenated 128-bit characteristic UUID.
  const std::string& Uuid() const;

  /// @brief Returns properties visible at discovery.
  GattCharacteristicProperties Properties() const;

  /// @brief Returns the descriptor list when the backend could enumerate it.
  /// An empty vector means enumeration completed and yielded no descriptors.
  const std::optional<std::vector<GattDescriptor>>& Descriptors() const;

  /// @brief Compares characteristic instance identity within the originating snapshot.
  bool operator==(const GattCharacteristic& other) const noexcept;

private:
  GattCharacteristic(std::shared_ptr<const detail::Attribute> attribute,
                     GattCharacteristicProperties properties, std::optional<std::vector<GattDescriptor>> descriptors);

  std::shared_ptr<const detail::Attribute> attribute_;
  GattCharacteristicProperties properties_;
  std::optional<std::vector<GattDescriptor>> descriptors_;
  friend struct detail::Access;
};

/// @brief A discovered GATT service and its characteristics. UUIDs can repeat in the returned service list.
struct GattService {
  /// The lowercase, hyphenated 128-bit service UUID.
  std::string uuid;
  std::vector<GattCharacteristic> characteristics;
};

/// @brief Lifecycle of an acquired logical GATT session.
enum class GattConnectionState {
  /// The session accepts GATT requests.
  Connected,
  /// The library is retiring the session and its pending work.
  Disconnecting,
  /// The logical session is retired. Connect again to obtain another session.
  Disconnected,
};

/// @brief The active discovery snapshot has been retired. Call DiscoverAsync() to resolve new attributes.
struct GattServicesChanged {};

/// @brief The logical session closed and the library can accept a fresh connection attempt.
struct GattDisconnected {
  /// Empty after an intentional disconnect. An unexpected disconnect carries the available platform error.
  std::optional<BluetoothError> error;
};

/// @brief Session events delivered on the owning HuxerUI application thread.
using GattEvent = std::variant<GattServicesChanged, GattDisconnected>;

/// @brief Unique owner of an application value listener. Use and destroy the owner on the application thread.
/// Destruction stops delivery to the listener.
class GattSubscription final {
public:
  GattSubscription(GattSubscription&& other) noexcept;

  GattSubscription& operator=(GattSubscription&& other) noexcept;

  ~GattSubscription();

  /// @brief Retires this listener immediately.
  /// When the last listener stops, the library queues a request to disable platform delivery for the characteristic.
  void Stop() noexcept;

private:
  explicit GattSubscription(std::shared_ptr<detail::Subscription> subscription);

  std::shared_ptr<detail::Subscription> subscription_;
  friend struct detail::Access;
};

/// @brief Unique owner of one logical GATT connection. Call its methods on the original application's thread.
/// Retain the session while using its attributes. Moving transfers ownership and destruction closes the session
/// and suppresses pending session events.
///
/// Operations start when their Task runs. Canceling or timing out an already dispatched GATT request closes the
/// logical session. A canceled Task scope detaches its continuation. Reconnect and discover attributes again.
class GattSession final {
public:
  GattSession(GattSession&& other) noexcept;

  GattSession& operator=(GattSession&& other) noexcept;

  ~GattSession();

  /// @brief Returns the current logical connection state.
  GattConnectionState State() const;

  /// @brief Reports the known maximum payload for a single characteristic write.
  /// @param mode Requested GATT write mode.
  /// @return The size in bytes, or an empty value when the backend cannot determine it.
  std::optional<std::size_t> MaxWriteSize(GattWriteMode mode) const;

  /// @brief Discovers visible services, characteristics, and descriptors.
  /// @param options Deadline for discovery.
  /// @return The new snapshot. Success retires attributes and subscriptions from previous discovery.
  Task<BluetoothResult<std::vector<GattService>>> DiscoverAsync(OperationOptions options = {});

  /// @brief Reads the current value of a characteristic.
  /// @param characteristic Readable characteristic from the current discovery snapshot.
  /// @param options Deadline for the read.
  /// @return Owned value bytes, including a valid empty value, or an operation error.
  /// On iOS, macOS, and BlueZ, stop an active subscription to this characteristic before reading it.
  Task<BluetoothResult<Bytes>> ReadAsync(GattCharacteristic characteristic, OperationOptions options = {});

  /// @brief Writes one characteristic value using the selected mode.
  /// @param characteristic Writable characteristic from the current discovery snapshot.
  /// @param value Owned application payload.
  /// @param mode WithResponse waits for GATT acknowledgement. WithoutResponse reports backend acceptance.
  /// @param options Deadline for the write.
  /// @return Success at the selected completion point, or an operation error.
  Task<BluetoothResult<void>> WriteAsync(GattCharacteristic characteristic, Bytes value, GattWriteMode mode,
                                        OperationOptions options = {});

  /// @brief Subscribes to notifications or indications from a characteristic.
  /// @param characteristic Characteristic with notification or indication support.
  /// @param on_value Nonempty callback receiving each owned update on the application thread.
  /// @param options Deadline for enabling delivery.
  /// @return A subscription on success or an operation error. Keep the subscription while updates are needed.
  Task<BluetoothResult<GattSubscription>> SubscribeAsync(GattCharacteristic characteristic,
                                                        std::function<void(Bytes)> on_value,
                                                        OperationOptions options = {});

  /// @brief Reads a remote descriptor.
  /// @param descriptor Descriptor from the current discovery snapshot.
  /// @param options Deadline for the read.
  /// @return Owned descriptor bytes or an operation error.
  Task<BluetoothResult<Bytes>> ReadDescriptorAsync(GattDescriptor descriptor, OperationOptions options = {});

  /// @brief Writes a remote descriptor. Use SubscribeAsync() to configure value delivery.
  /// @param descriptor Descriptor from the current discovery snapshot.
  /// @param value Owned descriptor payload.
  /// @param options Deadline for the write.
  /// @return Success after acknowledgement, or an operation error.
  Task<BluetoothResult<void>> WriteDescriptorAsync(GattDescriptor descriptor, Bytes value,
                                                  OperationOptions options = {});

  /// @brief Retires the session for new I/O and starts platform cleanup.
  /// Retain the session until the terminal event if it is needed. The event is queued after the library retires the
  /// connection slot. The platform can still be completing its cleanup.
  void Disconnect() noexcept;

private:
  explicit GattSession(std::shared_ptr<detail::Session> session);

  std::shared_ptr<detail::Session> session_;
  friend struct detail::Access;
};

/// @brief Application-scoped entry point for nearby Bluetooth devices.
/// Call its methods on the owning application thread, and launch operations from an event or committed lifecycle.
/// @code{.cpp}
/// auto bluetooth = huxerui::UseService<huxerui::bluetooth::Bluetooth>();
/// if (bluetooth->Capabilities().can_scan) {
///   auto result = bluetooth->StartScan({}, [](huxerui::bluetooth::ScanEvent event) {
///     ConsumeScanEvent(std::move(event));
///   });
///   if (result.Succeeded()) {
///     StoreScan(std::move(result).Value());
///   } else {
///     ShowBluetoothError(result.Error());
///   }
/// }
/// @endcode
class Bluetooth final {
public:
  /// @brief Returns supported operations for the current backend.
  BluetoothCapabilities Capabilities() const;

  /// @brief Returns the cached adapter status without querying the backend or presenting a permission prompt.
  AdapterStatus GetAdapterStatus() const;

  /// @brief Observes the current adapter status and subsequent distinct changes.
  /// @param on_status Nonempty callback invoked on the application thread.
  /// @return A lazy Task that completes when the service closes. Cancel the Task or its scope to stop watching.
  /// @throws std::invalid_argument If on_status is empty.
  Task<void> WatchAdapterStatusAsync(std::function<void(AdapterStatus)> on_status) const;

  /// @brief Admits a bounded foreground scan with one handler registered from the start.
  /// @param options Service filters and positive maximum scan duration.
  /// @param on_event Nonempty callback receiving observations and the terminal result on the application thread.
  /// @return A scan owner on admission. An existing foreground scan returns Busy. Invalid filters, duration, or
  /// callback return InvalidArgument. A later platform failure arrives through on_event.
  BluetoothResult<ScanSession> StartScan(ScanOptions options, std::function<void(ScanEvent)> on_event);

  /// @brief Starts the system chooser immediately, inside the originating user gesture.
  /// @param options Requested advertised and accessible services. Invalid filters return InvalidArgument.
  /// @return A Task awaiting the selected device or an error. Move it into a TaskScope.
  /// @code{.cpp}
  /// auto selection = bluetooth->SelectDevice({.advertised_services = {required_service}});
  /// tasks.Launch([selection = std::move(selection)]() mutable -> huxerui::Task<void> {
  ///   auto result = co_await std::move(selection);
  ///   if (result.Succeeded()) {
  ///     ShowDevice(result.Value());
  ///   } else {
  ///     ShowBluetoothError(result.Error());
  ///   }
  /// });
  /// @endcode
  Task<BluetoothResult<Device>> SelectDevice(SelectDeviceOptions options);

  /// @brief Resolves an opaque identifier through the original backend's device or grant store.
  /// @param id Copy of a previously observed Device::Id().
  /// @param options Deadline for recovery.
  /// @return The device when the backend still recognizes it, or an operation error.
  Task<BluetoothResult<Device>> RestoreDeviceAsync(std::string id, OperationOptions options = {});

  /// @brief Queries an observable system bond.
  /// @param device Device from this application service.
  /// @param options Deadline for the query.
  /// @return Bond state, including Unknown when the platform cannot establish it, or an operation error.
  Task<BluetoothResult<BondState>> GetBondStateAsync(Device device, OperationOptions options = {});

  /// @brief Starts the system pairing flow. Launch from an explicit application action.
  /// @param device Device from this application service.
  /// @param options Deadline for the application result.
  /// @return Success once the platform confirms pairing or an existing bond, or an operation error.
  Task<BluetoothResult<void>> PairAsync(Device device, OperationOptions options = {});

  /// @brief Opens a new GATT session. Connect again after disconnect to obtain a fresh session.
  /// @param device Device from this application service.
  /// @param options Deadline for connection.
  /// @param on_event Optional callback for service changes and terminal connection events.
  /// @return A connected session or an operation error. An active session or attempt for the same device returns Busy.
  ///
  /// This example runs in an application Task. The application resolves its characteristic from the current snapshot
  /// and retains both owners in its model.
  /// @code{.cpp}
  /// auto connected = co_await bluetooth->ConnectAsync(device, {}, OnGattEvent);
  /// if (!connected.Succeeded()) {
  ///   ShowBluetoothError(connected.Error());
  ///   co_return;
  /// }
  /// auto session = std::move(connected).Value();
  /// auto discovered = co_await session.DiscoverAsync();
  /// if (!discovered.Succeeded()) {
  ///   ShowBluetoothError(discovered.Error());
  ///   co_return;
  /// }
  /// if (auto characteristic = FindNotificationCharacteristic(discovered.Value())) {
  ///   auto listened = co_await session.SubscribeAsync(*characteristic, ConsumeValue);
  ///   if (listened.Succeeded()) {
  ///     StoreConnection(std::move(session), std::move(listened).Value());
  ///   } else {
  ///     ShowBluetoothError(listened.Error());
  ///   }
  /// } else {
  ///   ShowMissingCharacteristic();
  /// }
  /// @endcode
  Task<BluetoothResult<GattSession>> ConnectAsync(Device device, OperationOptions options = {},
                                                 std::function<void(GattEvent)> on_event = {});

private:
  explicit Bluetooth(std::shared_ptr<detail::Service> service);

  std::shared_ptr<detail::Service> service_;
  friend void Install(ApplicationContext& context);
};

/// @brief Registers the Bluetooth backend and publishes the application-scoped service.
/// @param context Application installation context supplied to the application hook.
/// @code{.cpp}
/// const huxerui::Application application{App, {.application_hooks = {huxerui::bluetooth::Install}}};
/// @endcode
void Install(ApplicationContext& context);

} // namespace huxerui::bluetooth
