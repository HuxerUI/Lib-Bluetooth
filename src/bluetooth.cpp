#include "backend.h"

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <utility>

namespace huxerui::bluetooth::detail {

using Code = BluetoothErrorCode;

BluetoothError Error(Code code, std::string message, std::string domain, std::optional<std::int64_t> platform_code) {
  return {code, std::move(message), std::move(domain), platform_code};
}

Reply Ok(ReplyValue value) {
  return Reply::Success(std::move(value));
}

Reply Fail(Code code, std::string message) {
  return Reply::Failure(Error(code, std::move(message)));
}

std::optional<std::string> NormalizeUuid(std::string_view input) {
  std::string uuid(input);
  if (uuid.size() == 4)
    uuid.insert(0, "0000");
  if (uuid.size() == 8)
    uuid += "-0000-1000-8000-00805f9b34fb";
  if (uuid.size() != 36)
    return std::nullopt;
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    char& letter = uuid[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (letter != '-')
        return std::nullopt;
      continue;
    }
    if (letter >= 'A' && letter <= 'F')
      letter = static_cast<char>(letter - 'A' + 'a');
    if ((letter < '0' || letter > '9') && (letter < 'a' || letter > 'f'))
      return std::nullopt;
  }
  return uuid;
}

namespace {

constexpr std::size_t max_pending_gatt_requests = 64;

bool NormalizeUuids(std::vector<std::string>& uuids) {
  for (auto& uuid : uuids) {
    auto canonical = NormalizeUuid(uuid);
    if (!canonical)
      return false;
    uuid = std::move(*canonical);
  }
  return true;
}

bool IsGattOperation(Operation operation) {
  return operation == Operation::Discover || operation == Operation::Read ||
         operation == Operation::Write || operation == Operation::Notify;
}

bool SameAdvertisement(const Advertisement& first, const Advertisement& second) {
  return first.local_name == second.local_name && first.service_uuids == second.service_uuids &&
         first.manufacturer_data == second.manufacturer_data && first.service_data == second.service_data &&
         first.rssi_dbm == second.rssi_dbm && first.connectable == second.connectable;
}

template <class T> BluetoothResult<T> Take(Reply reply) {
  if (!reply.Succeeded())
    return BluetoothResult<T>::Failure(std::move(reply).Error());
  if constexpr (std::is_void_v<T>) {
    return BluetoothResult<void>::Success();
  } else {
    if (auto* value = std::get_if<T>(&reply.Value()))
      return BluetoothResult<T>::Success(std::move(*value));
    return BluetoothResult<T>::Failure(Error(Code::PlatformFailure, "Unexpected Bluetooth result"));
  }
}

} // namespace

struct Attribute {
  std::weak_ptr<Session> session;
  Token snapshot;
  Token id;
  std::string uuid;
};

struct DeviceData {
  std::weak_ptr<Service> service;
  FoundDevice device;
};

// Dispatch retains the registered callable so a handler can stop its own registration while it runs.
template <class Event> using EventHandler = std::shared_ptr<std::function<void(Event)>>;

struct Scan {
  std::weak_ptr<Service> service;
  Token id;
  TaskHandle deadline;
  EventHandler<ScanEvent> handler;
  std::vector<std::string> filters;
  std::map<std::string, std::pair<std::optional<std::string>, Advertisement>> observations;
};

struct Subscription {
  std::weak_ptr<Session> session;
  Token attribute;
  Token snapshot;
  EventHandler<Bytes> handler;
  bool ready = false;
};

struct Session {
  std::weak_ptr<Service> service;
  Token id;
  std::string device;
  GattConnectionState status = GattConnectionState::Connected;
  EventHandler<GattEvent> handler;
  Token snapshot = 0;
  WriteLimits limits;
  Token active_request = 0;
  std::deque<Token> queued;
  std::multimap<Token, std::weak_ptr<Subscription>> listeners;
  std::set<Token> enabled_notifications;
  bool connect_complete = false;
};

struct AdapterWatch {
  EventHandler<AdapterStatus> handler;
  CallbackCompletion<void> end;
};

struct Access {
  static Device MakeDevice(const std::shared_ptr<Service>& service, FoundDevice value) {
    return Device(std::make_shared<DeviceData>(DeviceData{service, std::move(value)}));
  }

  static bool From(const Device& device, const std::shared_ptr<Service>& service) {
    return device.data_ && device.data_->service.lock() == service;
  }

  static ScanSession MakeScan(std::shared_ptr<Scan> scan) {
    return ScanSession(std::move(scan));
  }

  static GattSession MakeSession(std::shared_ptr<Session> session) {
    return GattSession(std::move(session));
  }

  static GattSubscription MakeSubscription(std::shared_ptr<Subscription> subscription) {
    return GattSubscription(std::move(subscription));
  }

  static const auto& Get(const GattCharacteristic& characteristic) {
    return characteristic.attribute_;
  }

  static const auto& Get(const GattDescriptor& descriptor) {
    return descriptor.attribute_;
  }

  static std::vector<GattService> MakeGattServices(const std::shared_ptr<Session>& owner, Discovery discovery) {
    std::vector<GattService> services;
    for (auto& service : discovery.services) {
      GattService output{NormalizeUuid(service.uuid).value_or(std::move(service.uuid)), {}};
      for (auto& characteristic : service.characteristics) {
        std::optional<std::vector<GattDescriptor>> descriptors;
        if (characteristic.descriptors) {
          descriptors.emplace();
          for (auto& descriptor : *characteristic.descriptors) {
            descriptors->push_back(GattDescriptor(std::make_shared<Attribute>(Attribute{
                owner, discovery.snapshot, descriptor.id, NormalizeUuid(descriptor.uuid).value_or(descriptor.uuid)})));
          }
        }
        auto attribute = std::make_shared<Attribute>(Attribute{
            owner, discovery.snapshot, characteristic.id, NormalizeUuid(characteristic.uuid).value_or(characteristic.uuid)});
        output.characteristics.push_back(
            GattCharacteristic(std::move(attribute), characteristic.properties, std::move(descriptors)));
      }
      services.push_back(std::move(output));
    }
    return services;
  }
};

struct Work {
  Request request;
  Completion done;
  TaskHandle deadline;
  Cancellation cancel;
  bool started = false;
};

struct Selection {
  std::optional<Reply> result;
  std::optional<CallbackCompletion<Reply>> waiter;

  void Set(Reply reply) {
    if (waiter)
      waiter->Complete(std::move(reply));
    else
      result.emplace(std::move(reply));
  }
};

struct Service : std::enable_shared_from_this<Service> {
  explicit Service(TaskScope scope) : tasks(std::move(scope)) {}

  ~Service() {
    if (scan)
      scan->deadline.Cancel();
    for (auto& [id, session] : links) {
      session->status = GattConnectionState::Disconnected;
      for (auto& [attribute, ref] : session->listeners) {
        if (auto listener = ref.lock())
          listener->handler = {};
      }
    }
    for (auto& [id, work] : requests) {
      work.deadline.Cancel();
      work.done(Fail(Code::Unavailable, "Bluetooth service closed"));
    }
    for (auto& [id, watch] : watches)
      watch.end.Complete();
    if (selection)
      selection->Set(Fail(Code::Unavailable, "Bluetooth service closed"));
  }

  void EnsureBackend() {
    if (backend || backend_failed)
      return;
    try {
      backend = CreateBackend(tasks, [weak = weak_from_this(), tasks = tasks](BackendEvent event) {
        tasks.Post([weak, event = std::move(event)]() mutable {
          if (auto service = weak.lock())
            std::visit([&](auto&& value) { service->Handle(std::move(value)); }, std::move(event));
        });
      });
    } catch (const std::exception&) {
      backend_failed = true;
    }
    Handle(backend ? backend->Status() : AdapterStatus::Unavailable);
  }

  BluetoothCapabilities Capabilities() const {
    return backend_failed ? BluetoothCapabilities{} : PlatformCapabilities();
  }

  std::optional<BluetoothError> Check(Operation operation) const {
    const auto caps = Capabilities();
    bool supported = caps.can_use_gatt_client;
    switch (operation) {
      case Operation::Scan: supported = caps.can_scan; break;
      case Operation::Select: supported = caps.can_select_device; break;
      case Operation::Restore: supported = caps.can_restore_device; break;
      case Operation::Bond: supported = caps.can_query_bond; break;
      case Operation::Pair: supported = caps.can_pair; break;
      default: break;
    }
    if (backend_failed)
      return Error(Code::Unavailable, "Bluetooth backend could not be opened");
    if (!supported)
      return Error(Code::Unsupported, "This Bluetooth operation is unavailable on the current platform");
    if (status == AdapterStatus::PoweredOff && operation != Operation::Restore && operation != Operation::Bond)
      return Error(Code::AdapterOff, "The Bluetooth adapter is off");
    if (!backend || status == AdapterStatus::Unavailable)
      return Error(Code::Unavailable, "The Bluetooth adapter is unavailable");
    return std::nullopt;
  }

  void Handle(AdapterStatus next) {
    if (status == next)
      return;
    status = next;
    if (next == AdapterStatus::PoweredOff || next == AdapterStatus::Unavailable) {
      auto error = Error(next == AdapterStatus::PoweredOff ? Code::AdapterOff : Code::Unavailable,
                         "The Bluetooth adapter became unavailable");
      if (scan)
        EndScan(scan->id, error, true);
      while (!links.empty())
        CloseLink(links.begin()->second, error, true);
    }
    for (const auto& [id, watch] : watches) {
      tasks.Post([weak = weak_from_this(), id, next] {
        if (auto service = weak.lock()) {
          const auto found = service->watches.find(id);
          if (found != service->watches.end()) {
            auto callback = found->second.handler;
            (*callback)(next);
          }
        }
      });
    }
  }

  void Handle(ScanReport report) {
    if (!scan || scan->id != report.scan || report.device.id.empty())
      return;
    for (auto& uuid : report.advertisement.service_uuids)
      uuid = NormalizeUuid(uuid).value_or(uuid);
    std::ranges::sort(report.advertisement.service_uuids);
    auto& uuids = report.advertisement.service_uuids;
    uuids.erase(std::unique(uuids.begin(), uuids.end()), uuids.end());
    if (!scan->filters.empty() && std::ranges::none_of(scan->filters, [&](const auto& uuid) {
          return std::ranges::binary_search(uuids, uuid);
        })) {
      return;
    }
    const auto old = scan->observations.find(report.device.id);
    if (old != scan->observations.end() && old->second.first == report.device.name &&
        SameAdvertisement(old->second.second, report.advertisement)) {
      return;
    }
    scan->observations.insert_or_assign(report.device.id, std::pair{report.device.name, report.advertisement});
    if (const auto handler = scan->handler) {
      (*handler)(ScanObservation{Access::MakeDevice(shared_from_this(), std::move(report.device)),
                                 std::move(report.advertisement)});
    }
  }

  void Handle(ScanFailure failure) {
    EndScan(failure.scan, std::move(failure.error), true);
  }

  void Handle(LinkLost event) {
    const auto found = links.find(event.link);
    if (found != links.end())
      CloseLink(found->second, std::move(event.error), true);
  }

  void Handle(ServicesChanged event) {
    const auto found = links.find(event.link);
    if (found == links.end())
      return;
    auto session = found->second;
    if (!session->connect_complete || session->snapshot == 0)
      return;
    RetireSnapshot(*session);
    const auto error = Error(Code::StaleAttribute, "The remote GATT database changed");
    std::vector<Token> stale;
    for (const auto& [id, work] : requests) {
      if (work.request.link == event.link && work.request.snapshot != 0)
        stale.push_back(id);
    }
    const auto active = session->active_request;
    const bool retire_link = std::ranges::find(stale, active) != stale.end();
    if (retire_link) {
      CloseLink(session, error, true);
      return;
    }
    for (auto id : stale)
      Finish(id, Reply::Failure(error));
    if (const auto handler = session->handler)
      (*handler)(GattServicesChanged{});
  }

  void Handle(LimitsChanged event) {
    if (const auto found = links.find(event.link); found != links.end())
      found->second->limits = event.limits;
  }

  void Handle(ValueChanged event) {
    const auto found = links.find(event.link);
    if (found == links.end())
      return;
    auto& session = *found->second;
    auto [first, last] = session.listeners.equal_range(event.attribute);
    std::vector<std::shared_ptr<Subscription>> listeners;
    for (auto iter = first; iter != last; ++iter) {
      if (auto listener = iter->second.lock(); listener && listener->ready)
        listeners.push_back(std::move(listener));
    }
    for (const auto& listener : listeners) {
      if (const auto callback = listener->handler)
        (*callback)(event.value);
    }
  }

  void EndScan(Token id, ScanEvent event, bool notify) {
    if (!scan || scan->id != id)
      return;
    auto finished = std::exchange(scan, {});
    finished->deadline.Cancel();
    if (backend)
      backend->StopScan(id);
    if (notify) {
      tasks.Post([weak = std::weak_ptr<Scan>(finished), event = std::move(event)]() mutable {
        if (auto owner = weak.lock(); owner && owner->handler) {
          auto callback = std::exchange(owner->handler, {});
          (*callback)(std::move(event));
        }
      });
    }
  }

  void RetireSnapshot(Session& session) {
    session.snapshot = 0;
    for (auto& [attribute, ref] : session.listeners)
      if (auto listener = ref.lock())
        listener->handler = {};
    session.listeners.clear();
    session.enabled_notifications.clear();
  }

  void CloseLink(std::shared_ptr<Session> session, std::optional<BluetoothError> error, bool notify) {
    if (session->status == GattConnectionState::Disconnected)
      return;
    session->status = GattConnectionState::Disconnecting;
    links.erase(session->id);
    RetireSnapshot(*session);
    if (backend)
      backend->Disconnect(session->id);

    std::vector<Token> retired;
    for (const auto& [id, work] : requests)
      if (work.request.link == session->id)
        retired.push_back(id);
    for (const auto id : retired) {
      auto& work = requests.at(id);
      if (work.cancel)
        work.cancel();
      Finish(id, Reply::Failure(error.value_or(Error(Code::Disconnected, "GATT session closed"))));
    }

    session->status = GattConnectionState::Disconnected;
    if (notify && session->connect_complete && session->handler) {
      tasks.Post([weak = std::weak_ptr<Session>(session), error = std::move(error)]() mutable {
        if (auto owner = weak.lock(); owner && owner->handler) {
          auto callback = std::exchange(owner->handler, {});
          (*callback)(GattDisconnected{std::move(error)});
        }
      });
    }
  }

  void Dispatch(Token id) {
    auto found = requests.find(id);
    if (found == requests.end())
      return;
    auto& work = found->second;
    const auto& request = work.request;
    if (IsGattOperation(request.operation)) {
      auto session = links.at(request.link);
      if (request.snapshot != 0 && request.snapshot != session->snapshot) {
        Finish(id, Fail(Code::StaleAttribute, "The attribute no longer belongs to the active GATT snapshot"));
        return;
      }
      if (request.operation == Operation::Notify) {
        const bool enabled = session->enabled_notifications.contains(request.attribute);
        if (!request.enabled && session->listeners.contains(request.attribute))
          work.request.enabled = enabled;
        if (work.request.enabled == enabled) {
          Finish(id, Ok());
          return;
        }
      }
    }
    work.started = true;
    try {
      work.cancel = backend->Invoke(request, [weak = weak_from_this(), tasks = tasks, id](Reply reply) mutable {
        tasks.Post([weak, id, reply = std::move(reply)]() mutable {
          if (auto service = weak.lock())
            service->Finish(id, std::move(reply));
        });
      });
    } catch (const std::exception& error) {
      Finish(id, Fail(Code::PlatformFailure, error.what()));
    }
  }

  void Pump(const std::shared_ptr<Session>& session) {
    if (session->active_request || session->status != GattConnectionState::Connected)
      return;
    while (!session->queued.empty()) {
      const auto id = session->queued.front();
      session->queued.pop_front();
      if (requests.contains(id)) {
        session->active_request = id;
        Dispatch(id);
        return;
      }
    }
  }

  void Finish(Token id, Reply reply, bool abort = false) {
    auto node = requests.extract(id);
    if (node.empty())
      return;
    Work& work = node.mapped();
    work.deadline.Cancel();
    if (abort && work.cancel)
      work.cancel();

    std::shared_ptr<Session> session;
    if (const auto found = links.find(work.request.link); found != links.end())
      session = found->second;
    if (session) {
      if (session->active_request == id)
        session->active_request = 0;
      else
        std::erase(session->queued, id);
      if (reply.Succeeded() && work.request.operation == Operation::Discover) {
        if (auto* discovery = std::get_if<Discovery>(&reply.Value())) {
          RetireSnapshot(*session);
          discovery->snapshot = session->snapshot = next++;
        }
      }
      if (reply.Succeeded() && work.request.operation == Operation::Notify) {
        if (work.request.enabled)
          session->enabled_notifications.insert(work.request.attribute);
        else
          session->enabled_notifications.erase(work.request.attribute);
      }
      if (abort && work.started) {
        CloseLink(session, Error(Code::Disconnected, "Session closed after an interrupted GATT request"), true);
      }
    }
    work.done(std::move(reply));
    if (session)
      Pump(session);
  }

  Cancellation Run(Request request, OperationOptions options, CallbackCompletion<Reply> done) {
    EnsureBackend();
    if (auto error = Check(request.operation)) {
      done.Complete(Reply::Failure(std::move(*error)));
      return {};
    }
    if (options.timeout <= std::chrono::milliseconds::zero()) {
      done.Complete(Fail(Code::InvalidArgument, "The operation timeout must be positive"));
      return {};
    }
    std::shared_ptr<Session> session;
    if (IsGattOperation(request.operation)) {
      const auto found = links.find(request.link);
      if (found == links.end() || !found->second->connect_complete) {
        done.Complete(Fail(Code::Disconnected, "The GATT session is closed"));
        return {};
      }
      session = found->second;
      if (session->queued.size() >= max_pending_gatt_requests) {
        done.Complete(Fail(Code::Busy, "The GATT operation queue is full"));
        return {};
      }
    }
    const auto id = next++;
    auto weak = weak_from_this();
    auto deadline = tasks.Launch([weak, id, timeout = options.timeout]() -> Task<void> {
      co_await Delay(timeout);
      if (auto service = weak.lock())
        service->Finish(id, Fail(Code::TimedOut, "The Bluetooth operation timed out"), true);
    });
    requests.emplace(id, Work{std::move(request), [done](Reply reply) { done.Complete(std::move(reply)); },
                              std::move(deadline), {}});
    if (session) {
      session->queued.push_back(id);
      Pump(session);
    } else {
      Dispatch(id);
    }
    return [weak, id] {
      if (auto service = weak.lock())
        service->Finish(id, Fail(Code::Canceled, "The Bluetooth operation was canceled"), true);
    };
  }

  TaskScope tasks;
  std::unique_ptr<Backend> backend;
  AdapterStatus status = AdapterStatus::Unknown;
  bool backend_failed = false;
  Token next = 1;
  std::map<Token, AdapterWatch> watches;
  std::shared_ptr<Scan> scan;
  std::map<Token, std::shared_ptr<Session>> links;
  std::map<Token, Work> requests;
  std::shared_ptr<Selection> selection;
};

namespace {

Task<Reply> Run(std::weak_ptr<Service> weak, Request request, OperationOptions options) {
  return AwaitCallback<Reply>([weak, request = std::move(request), options](auto done) mutable -> Cancellation {
    if (auto service = weak.lock())
      return service->Run(std::move(request), options, done);
    done.Complete(Fail(Code::Unavailable, "Bluetooth service closed"));
    return {};
  });
}

template <class T> Task<BluetoothResult<T>> AttributeOperation(std::shared_ptr<Session> session,
                                                               std::shared_ptr<const Attribute> attribute,
                                                               Request request, OperationOptions options) {
  if (!session || session->status != GattConnectionState::Connected)
    co_return BluetoothResult<T>::Failure(Error(Code::Disconnected, "The GATT session is closed"));
  if (!attribute || attribute->session.lock() != session || attribute->snapshot != session->snapshot)
    co_return BluetoothResult<T>::Failure(Error(Code::StaleAttribute, "The attribute is not in this GATT snapshot"));
  request.link = session->id;
  request.attribute = attribute->id;
  request.snapshot = attribute->snapshot;
  co_return Take<T>(co_await Run(session->service, std::move(request), options));
}

template <class T> Task<BluetoothResult<T>> Reject(Code code, std::string message) {
  co_return BluetoothResult<T>::Failure(Error(code, std::move(message)));
}

Task<BluetoothResult<Device>> AwaitSelection(std::weak_ptr<Service> weak, std::shared_ptr<Selection> selection) {
  auto result = Take<FoundDevice>(co_await AwaitCallback<Reply>([selection](auto done) -> Cancellation {
    if (selection->result)
      done.Complete(std::move(*selection->result));
    else
      selection->waiter = done;
    return {};
  }));
  auto service = weak.lock();
  if (!result.Succeeded())
    co_return BluetoothResult<Device>::Failure(std::move(result).Error());
  if (!service)
    co_return BluetoothResult<Device>::Failure(Error(Code::Unavailable, "Bluetooth service closed"));
  co_return BluetoothResult<Device>::Success(Access::MakeDevice(service, std::move(result).Value()));
}

Task<BluetoothResult<Device>> Restore(std::weak_ptr<Service> weak, std::string id, OperationOptions options) {
  if (id.empty())
    co_return BluetoothResult<Device>::Failure(Error(Code::InvalidArgument, "The device identifier is empty"));
  auto result = Take<FoundDevice>(co_await Run(weak, Request{.operation = Operation::Restore, .device = std::move(id)},
                                             options));
  if (!result.Succeeded())
    co_return BluetoothResult<Device>::Failure(std::move(result).Error());
  auto service = weak.lock();
  if (!service)
    co_return BluetoothResult<Device>::Failure(Error(Code::Unavailable, "Bluetooth service closed"));
  co_return BluetoothResult<Device>::Success(Access::MakeDevice(service, std::move(result).Value()));
}

template <class T> Task<BluetoothResult<T>> DeviceOperation(std::weak_ptr<Service> weak, Device device,
                                                           Operation operation, OperationOptions options) {
  auto service = weak.lock();
  if (!service || !Access::From(device, service))
    co_return BluetoothResult<T>::Failure(Error(Code::DeviceUnavailable, "The device belongs to another backend"));
  service.reset();
  co_return Take<T>(co_await Run(weak, Request{.operation = operation, .device = device.Id()}, options));
}

Task<BluetoothResult<GattSession>> Connect(std::weak_ptr<Service> weak, Device device, OperationOptions options,
                                           std::function<void(GattEvent)> on_event) {
  auto service = weak.lock();
  if (!service || !Access::From(device, service))
    co_return BluetoothResult<GattSession>::Failure(Error(Code::DeviceUnavailable, "The device is unavailable"));
  if (std::ranges::any_of(service->links, [&](const auto& entry) { return entry.second->device == device.Id(); }))
    co_return BluetoothResult<GattSession>::Failure(Error(Code::Busy, "The device already has a GATT session"));
  auto session = std::make_shared<Session>();
  session->service = service;
  session->id = service->next++;
  session->device = device.Id();
  if (on_event)
    session->handler = std::make_shared<decltype(on_event)>(std::move(on_event));
  service->links.emplace(session->id, session);
  service.reset();
  auto owner = Access::MakeSession(session);
  auto result = co_await Run(weak, Request{.operation = Operation::Connect, .link = session->id,
                                          .device = session->device}, options);
  if (!result.Succeeded())
    co_return BluetoothResult<GattSession>::Failure(std::move(result).Error());
  if (session->status == GattConnectionState::Disconnected)
    co_return BluetoothResult<GattSession>::Failure(Error(Code::Disconnected, "The device disconnected during setup"));
  if (auto* limits = std::get_if<WriteLimits>(&result.Value()))
    session->limits = *limits;
  session->connect_complete = true;
  co_return BluetoothResult<GattSession>::Success(std::move(owner));
}

Task<BluetoothResult<std::vector<GattService>>> Discover(std::shared_ptr<Session> session, OperationOptions options) {
  using Result = BluetoothResult<std::vector<GattService>>;
  if (!session || session->status != GattConnectionState::Connected)
    co_return Result::Failure(Error(Code::Disconnected, "The GATT session is closed"));
  auto result = Take<Discovery>(co_await Run(session->service,
                                            Request{.operation = Operation::Discover, .link = session->id}, options));
  if (!result.Succeeded())
    co_return Result::Failure(std::move(result).Error());
  if (result.Value().snapshot != session->snapshot)
    co_return Result::Failure(Error(Code::StaleAttribute, "The discovered GATT snapshot has already changed"));
  co_return Result::Success(Access::MakeGattServices(session, std::move(result).Value()));
}

Task<BluetoothResult<GattSubscription>> Subscribe(std::shared_ptr<Session> session, GattCharacteristic characteristic,
                                                  std::function<void(Bytes)> callback, OperationOptions options) {
  using Result = BluetoothResult<GattSubscription>;
  if (!callback)
    co_return Result::Failure(Error(Code::InvalidArgument, "A value callback is required"));
  const auto properties = characteristic.Properties();
  if (!properties.can_notify && !properties.can_indicate)
    co_return Result::Failure(Error(Code::UnsupportedProperty, "The characteristic cannot send value updates"));
  const auto attribute = Access::Get(characteristic);
  if (!session || !attribute || attribute->session.lock() != session || attribute->snapshot != session->snapshot)
    co_return Result::Failure(Error(Code::StaleAttribute, "The characteristic is not in this GATT snapshot"));
  auto subscription = std::make_shared<Subscription>(Subscription{
      session, attribute->id, attribute->snapshot, std::make_shared<decltype(callback)>(std::move(callback))});
  auto owner = Access::MakeSubscription(subscription);
  session->listeners.emplace(attribute->id, subscription);
  auto result = co_await AttributeOperation<void>(session, attribute,
                                                   Request{.operation = Operation::Notify, .enabled = true}, options);
  if (!result.Succeeded())
    co_return Result::Failure(std::move(result).Error());
  if (session->snapshot != subscription->snapshot)
    co_return Result::Failure(Error(Code::StaleAttribute, "The characteristic was invalidated during subscription"));
  subscription->ready = true;
  co_return Result::Success(std::move(owner));
}

void ReleaseSubscription(std::shared_ptr<Subscription> subscription) {
  if (!subscription || !subscription->handler)
    return;
  subscription->handler = {};
  auto session = subscription->session.lock();
  if (!session || session->snapshot != subscription->snapshot)
    return;
  auto [first, last] = session->listeners.equal_range(subscription->attribute);
  for (auto iter = first; iter != last;) {
    if (auto listener = iter->second.lock(); !listener || listener == subscription)
      iter = session->listeners.erase(iter);
    else
      ++iter;
  }
  if (!session->listeners.contains(subscription->attribute)) {
    if (auto service = session->service.lock()) {
      service->tasks.Post([weak = std::weak_ptr<Service>(service),
                           request = Request{.operation = Operation::Notify, .link = session->id,
                                             .attribute = subscription->attribute, .snapshot = subscription->snapshot}] {
        if (auto owner = weak.lock()) {
          owner->tasks.Launch([weak, request]() -> Task<void> {
            const auto result = co_await Run(weak, request, {});
            if (!result.Succeeded() && result.Error().code == Code::Busy) {
              if (auto owner = weak.lock()) {
                const auto found = owner->links.find(request.link);
                if (found != owner->links.end())
                  owner->CloseLink(found->second, result.Error(), true);
              }
            }
          });
        }
      });
    }
  }
}

void ReleaseSession(const std::shared_ptr<Session>& session, bool notify) {
  if (!session)
    return;
  if (!notify)
    session->handler = {};
  if (auto service = session->service.lock())
    service->CloseLink(session, {}, notify);
}

void ReleaseScan(const std::shared_ptr<Scan>& scan, bool notify) {
  if (!scan)
    return;
  if (!notify)
    scan->handler = {};
  if (auto service = scan->service.lock())
    service->EndScan(scan->id, ScanEndReason::Stopped, notify);
}

} // namespace
} // namespace huxerui::bluetooth::detail

namespace huxerui::bluetooth {

using detail::Access;
using detail::Code;
using detail::Error;
using detail::Operation;
using detail::Request;

Device::Device(std::shared_ptr<const detail::DeviceData> data) : data_(std::move(data)) {}

const std::string& Device::Id() const {
  return data_->device.id;
}

const std::optional<std::string>& Device::Name() const {
  return data_->device.name;
}

bool Device::operator==(const Device& other) const noexcept {
  if (data_ == other.data_)
    return true;
  return data_ && other.data_ && Id() == other.Id() &&
         !data_->service.owner_before(other.data_->service) && !other.data_->service.owner_before(data_->service);
}

ScanSession::ScanSession(std::shared_ptr<detail::Scan> scan) : scan_(std::move(scan)) {}

ScanSession::ScanSession(ScanSession&& other) noexcept = default;

ScanSession& ScanSession::operator=(ScanSession&& other) noexcept {
  if (this != &other) {
    detail::ReleaseScan(scan_, false);
    scan_ = std::move(other.scan_);
  }
  return *this;
}

ScanSession::~ScanSession() {
  detail::ReleaseScan(scan_, false);
}

void ScanSession::Stop() noexcept {
  detail::ReleaseScan(scan_, true);
}

GattDescriptor::GattDescriptor(std::shared_ptr<const detail::Attribute> attribute) : attribute_(std::move(attribute)) {}

const std::string& GattDescriptor::Uuid() const {
  return attribute_->uuid;
}

bool GattDescriptor::operator==(const GattDescriptor& other) const noexcept {
  return attribute_ == other.attribute_;
}

GattCharacteristic::GattCharacteristic(std::shared_ptr<const detail::Attribute> attribute,
                                       GattCharacteristicProperties properties,
                                       std::optional<std::vector<GattDescriptor>> descriptors)
    : attribute_(std::move(attribute)), properties_(properties), descriptors_(std::move(descriptors)) {}

const std::string& GattCharacteristic::Uuid() const {
  return attribute_->uuid;
}

GattCharacteristicProperties GattCharacteristic::Properties() const {
  return properties_;
}

const std::optional<std::vector<GattDescriptor>>& GattCharacteristic::Descriptors() const {
  return descriptors_;
}

bool GattCharacteristic::operator==(const GattCharacteristic& other) const noexcept {
  return attribute_ == other.attribute_;
}

GattSubscription::GattSubscription(std::shared_ptr<detail::Subscription> subscription)
    : subscription_(std::move(subscription)) {}

GattSubscription::GattSubscription(GattSubscription&& other) noexcept = default;

GattSubscription& GattSubscription::operator=(GattSubscription&& other) noexcept {
  if (this != &other) {
    Stop();
    subscription_ = std::move(other.subscription_);
  }
  return *this;
}

GattSubscription::~GattSubscription() {
  Stop();
}

void GattSubscription::Stop() noexcept {
  detail::ReleaseSubscription(std::exchange(subscription_, {}));
}

GattSession::GattSession(std::shared_ptr<detail::Session> session) : session_(std::move(session)) {}

GattSession::GattSession(GattSession&& other) noexcept = default;

GattSession& GattSession::operator=(GattSession&& other) noexcept {
  if (this != &other) {
    detail::ReleaseSession(session_, false);
    session_ = std::move(other.session_);
  }
  return *this;
}

GattSession::~GattSession() {
  detail::ReleaseSession(session_, false);
}

GattConnectionState GattSession::State() const {
  return session_ ? session_->status : GattConnectionState::Disconnected;
}

std::optional<std::size_t> GattSession::MaxWriteSize(GattWriteMode mode) const {
  if (State() != GattConnectionState::Connected)
    return std::nullopt;
  return mode == GattWriteMode::WithResponse ? session_->limits.with_response : session_->limits.without_response;
}

Task<BluetoothResult<std::vector<GattService>>> GattSession::DiscoverAsync(OperationOptions options) {
  return detail::Discover(session_, options);
}

Task<BluetoothResult<Bytes>> GattSession::ReadAsync(GattCharacteristic characteristic, OperationOptions options) {
  if (!characteristic.Properties().can_read)
    return detail::Reject<Bytes>(Code::UnsupportedProperty, "The characteristic is not readable");
  return detail::AttributeOperation<Bytes>(session_, Access::Get(characteristic),
                                           Request{.operation = Operation::Read}, options);
}

Task<BluetoothResult<void>> GattSession::WriteAsync(GattCharacteristic characteristic, Bytes value, GattWriteMode mode,
                                                   OperationOptions options) {
  const auto props = characteristic.Properties();
  if (mode == GattWriteMode::WithResponse ? !props.can_write_with_response : !props.can_write_without_response)
    return detail::Reject<void>(Code::UnsupportedProperty, "The characteristic does not support this write mode");
  if (const auto limit = MaxWriteSize(mode); limit && value.size() > *limit)
    return detail::Reject<void>(Code::PayloadTooLarge, "The value exceeds the current single-write limit");
  return detail::AttributeOperation<void>(session_, Access::Get(characteristic),
                                          Request{.operation = Operation::Write, .write_mode = mode,
                                                  .value = std::move(value)}, options);
}

Task<BluetoothResult<GattSubscription>> GattSession::SubscribeAsync(GattCharacteristic characteristic,
                                                                   std::function<void(Bytes)> on_value,
                                                                   OperationOptions options) {
  return detail::Subscribe(session_, std::move(characteristic), std::move(on_value), options);
}

Task<BluetoothResult<Bytes>> GattSession::ReadDescriptorAsync(GattDescriptor descriptor, OperationOptions options) {
  return detail::AttributeOperation<Bytes>(session_, Access::Get(descriptor),
                                           Request{.operation = Operation::Read, .descriptor = true}, options);
}

Task<BluetoothResult<void>> GattSession::WriteDescriptorAsync(GattDescriptor descriptor, Bytes value,
                                                             OperationOptions options) {
  const auto& attribute = Access::Get(descriptor);
  if (attribute && attribute->uuid == "00002902-0000-1000-8000-00805f9b34fb")
    return detail::Reject<void>(Code::UnsupportedProperty, "Use a subscription to configure characteristic updates");
  return detail::AttributeOperation<void>(session_, attribute,
                                          Request{.operation = Operation::Write, .descriptor = true,
                                                  .value = std::move(value)}, options);
}

void GattSession::Disconnect() noexcept {
  detail::ReleaseSession(session_, true);
}

Bluetooth::Bluetooth(std::shared_ptr<detail::Service> service) : service_(std::move(service)) {}

BluetoothCapabilities Bluetooth::Capabilities() const {
  return service_->Capabilities();
}

AdapterStatus Bluetooth::GetAdapterStatus() const {
  return service_->status;
}

Task<void> Bluetooth::WatchAdapterStatusAsync(std::function<void(AdapterStatus)> on_status) const {
  if (!on_status)
    throw std::invalid_argument("A Bluetooth adapter status callback is required");
  return AwaitCallback<void>([weak = std::weak_ptr<detail::Service>(service_), on_status = std::move(on_status)]
                            (auto done) mutable -> detail::Cancellation {
    auto service = weak.lock();
    if (!service) {
      done.Complete();
      return {};
    }
    service->EnsureBackend();
    auto handler = std::make_shared<decltype(on_status)>(std::move(on_status));
    (*handler)(service->status);
    auto id = service->next++;
    service->watches.emplace(id, detail::AdapterWatch{std::move(handler), done});
    return [weak, id] {
      if (auto owner = weak.lock())
        owner->watches.erase(id);
    };
  });
}

BluetoothResult<ScanSession> Bluetooth::StartScan(ScanOptions options, std::function<void(ScanEvent)> on_event) {
  using Result = BluetoothResult<ScanSession>;
  if (!on_event || options.duration <= std::chrono::milliseconds::zero() ||
      !detail::NormalizeUuids(options.advertised_services)) {
    return Result::Failure(Error(Code::InvalidArgument, "Invalid Bluetooth scan options or handler"));
  }
  service_->EnsureBackend();
  if (auto error = service_->Check(Operation::Scan))
    return Result::Failure(std::move(*error));
  if (service_->scan)
    return Result::Failure(Error(Code::Busy, "A Bluetooth scan is already active"));
  auto scan = std::make_shared<detail::Scan>();
  scan->service = service_;
  scan->id = service_->next++;
  scan->handler = std::make_shared<decltype(on_event)>(std::move(on_event));
  scan->filters = std::move(options.advertised_services);
  service_->scan = scan;
  auto weak = std::weak_ptr<detail::Service>(service_);
  scan->deadline = service_->tasks.Launch([weak, id = scan->id, duration = options.duration]() -> Task<void> {
    co_await Delay(duration);
    if (auto service = weak.lock())
      service->EndScan(id, ScanEndReason::DurationElapsed, true);
  });
  try {
    service_->backend->Invoke(Request{.operation = Operation::Scan, .scan = scan->id,
                                       .advertised_services = scan->filters},
                              [weak, tasks = service_->tasks, id = scan->id](detail::Reply result) {
      if (!result.Succeeded()) {
        tasks.Post([weak, id, error = std::move(result).Error()]() mutable {
          if (auto service = weak.lock())
            service->EndScan(id, std::move(error), true);
        });
      }
    });
  } catch (const std::exception& error) {
    service_->EndScan(scan->id, Error(Code::PlatformFailure, error.what()), false);
    return Result::Failure(Error(Code::PlatformFailure, error.what()));
  }
  return Result::Success(Access::MakeScan(std::move(scan)));
}

Task<BluetoothResult<Device>> Bluetooth::SelectDevice(SelectDeviceOptions options) {
  if (options.accept_all_devices != options.advertised_services.empty() ||
      !detail::NormalizeUuids(options.advertised_services) || !detail::NormalizeUuids(options.optional_services)) {
    return detail::Reject<Device>(Code::InvalidArgument, "Invalid Bluetooth device selection filters");
  }
  service_->EnsureBackend();
  auto selection = std::make_shared<detail::Selection>();
  auto weak = std::weak_ptr<detail::Service>(service_);
  if (auto error = service_->Check(Operation::Select)) {
    selection->Set(detail::Reply::Failure(std::move(*error)));
  } else if (service_->selection) {
    selection->Set(detail::Fail(Code::Busy, "A device chooser is already active"));
  } else {
    service_->selection = selection;
    try {
      service_->backend->Invoke(Request{.operation = Operation::Select,
                                         .advertised_services = std::move(options.advertised_services),
                                         .optional_services = std::move(options.optional_services)},
                                [weak, tasks = service_->tasks, selection](detail::Reply result) {
        tasks.Post([weak, selection, result = std::move(result)]() mutable {
          if (auto service = weak.lock(); service && service->selection == selection) {
            service->selection.reset();
            selection->Set(std::move(result));
          }
        });
      });
    } catch (const std::exception& error) {
      service_->selection.reset();
      selection->Set(detail::Fail(Code::PlatformFailure, error.what()));
    }
  }
  return detail::AwaitSelection(weak, std::move(selection));
}

Task<BluetoothResult<Device>> Bluetooth::RestoreDeviceAsync(std::string id, OperationOptions options) {
  return detail::Restore(service_, std::move(id), options);
}

Task<BluetoothResult<BondState>> Bluetooth::GetBondStateAsync(Device device, OperationOptions options) {
  return detail::DeviceOperation<BondState>(service_, std::move(device), Operation::Bond, options);
}

Task<BluetoothResult<void>> Bluetooth::PairAsync(Device device, OperationOptions options) {
  return detail::DeviceOperation<void>(service_, std::move(device), Operation::Pair, options);
}

Task<BluetoothResult<GattSession>> Bluetooth::ConnectAsync(Device device, OperationOptions options,
                                                          std::function<void(GattEvent)> on_event) {
  return detail::Connect(service_, std::move(device), options, std::move(on_event));
}

void Install(ApplicationContext& context) {
  detail::RegisterPlatform(context);
  auto service = std::make_shared<detail::Service>(UseApplicationTaskScope());
  context.Provide(std::shared_ptr<Bluetooth>(new Bluetooth(service)));
  const auto weak = std::weak_ptr<detail::Service>(service);
  service->tasks.Post([weak] {
    if (auto current = weak.lock())
      current->EnsureBackend();
  });
  context.OnLifecycleChanged([weak](ApplicationLifecycleState state) {
    if (auto current = weak.lock(); state == ApplicationLifecycleState::Background && current && current->scan)
      current->EndScan(current->scan->id, ScanEndReason::Stopped, true);
  });
}

} // namespace huxerui::bluetooth
