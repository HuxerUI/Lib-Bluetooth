package org.huxerui.lib.bluetooth;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.ParcelUuid;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.PlatformPayload;

import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

import static org.huxerui.lib.bluetooth.BluetoothModule.EMPTY;
import static org.huxerui.lib.bluetooth.BluetoothModule.CLIENT_CONFIGURATION;
import static org.huxerui.lib.bluetooth.BluetoothModule.copy;
import static org.huxerui.lib.bluetooth.BluetoothModule.fail;
import static org.huxerui.lib.bluetooth.BluetoothModule.failure;
import static org.huxerui.lib.bluetooth.BluetoothModule.id;
import static org.huxerui.lib.bluetooth.BluetoothModule.number;
import static org.huxerui.lib.bluetooth.BluetoothModule.object;

@SuppressWarnings("deprecation")
final class Peripheral {
    @FunctionalInterface
    interface Emitter {
        void emit(String type, long token, PlatformPayload payload);
    }

    private final Context context;
    private final BluetoothAdapter adapter;
    private final Handler handler;
    private final Emitter emitter;
    private Advertisement advertisement;
    private Server server;

    Peripheral(Context context, BluetoothAdapter adapter, Handler handler, Emitter emitter) {
        this.context = context;
        this.adapter = adapter;
        this.handler = handler;
        this.emitter = emitter;
    }

    PlatformPayload capabilities(boolean hasAdapter) {
        boolean advertising = hasAdapter && adapter.isMultipleAdvertisementSupported();
        return object("advertise", PlatformPayload.booleanValue(advertising),
                "server", PlatformPayload.booleanValue(hasAdapter));
    }

    HuxerUIPlatformChannel.Cancellation startAdvertising(PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        if (advertisement != null) {
            fail(result, "busy", "An Android advertisement is already active", 0);
            return null;
        }
        BluetoothLeAdvertiser advertiser = adapter.getBluetoothLeAdvertiser();
        if (advertiser == null) {
            fail(result, "unsupported", "Android BLE advertising is unavailable", 0);
            return null;
        }

        PlatformPayload name = args.requireField("local_name");
        if (!name.isNull() && !name.requireString().equals(adapter.getName())) {
            fail(result, "unsupported", "Android can only advertise the adapter's current name", 0);
            return null;
        }
        AdvertiseData.Builder data = new AdvertiseData.Builder().setIncludeDeviceName(!name.isNull());
        for (PlatformPayload uuid : args.requireField("services").elements()) {
            data.addServiceUuid(ParcelUuid.fromString(uuid.requireString()));
        }
        PlatformPayload mode = args.requireField("connectable");
        boolean connectable = mode.isNull() || mode.requireBoolean();
        AdvertiseSettings settings = new AdvertiseSettings.Builder().setConnectable(connectable).build();
        Advertisement opened = new Advertisement(id(args, "peripheral"), advertiser, result);
        advertisement = opened;
        try {
            advertiser.startAdvertising(settings, data.build(), opened);
        } catch (RuntimeException error) {
            stopAdvertising(opened.token);
            throw error;
        }
        return () -> stopAdvertising(opened.token);
    }

    void stopAdvertising(long token) {
        if (advertisement == null || advertisement.token != token)
            return;
        Advertisement stopped = advertisement;
        advertisement = null;
        try {
            stopped.advertiser.stopAdvertising(stopped);
        } catch (SecurityException ignored) {
            // The platform already removed advertising access.
        }
    }

    private final class Advertisement extends AdvertiseCallback {
        final long token;
        final BluetoothLeAdvertiser advertiser;
        final HuxerUIPlatformChannel.Result result;

        Advertisement(long token, BluetoothLeAdvertiser advertiser, HuxerUIPlatformChannel.Result result) {
            this.token = token;
            this.advertiser = advertiser;
            this.result = result;
        }

        @Override
        public void onStartSuccess(AdvertiseSettings settings) {
            handler.post(() -> {
                if (advertisement == this)
                    result.complete(EMPTY);
            });
        }

        @Override
        public void onStartFailure(int status) {
            handler.post(() -> {
                if (advertisement != this)
                    return;
                advertisement = null;
                String code;
                switch (status) {
                    case ADVERTISE_FAILED_DATA_TOO_LARGE: code = "length"; break;
                    case ADVERTISE_FAILED_TOO_MANY_ADVERTISERS:
                    case ADVERTISE_FAILED_ALREADY_STARTED: code = "busy"; break;
                    case ADVERTISE_FAILED_FEATURE_UNSUPPORTED: code = "unsupported"; break;
                    default: code = "platform";
                }
                fail(result, code, "Android BLE advertising failed", status);
            });
        }
    }

    HuxerUIPlatformChannel.Cancellation openServer(PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        if (server != null) {
            fail(result, "busy", "An Android GATT server is already open", 0);
            return null;
        }
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        if (manager == null) {
            fail(result, "unavailable", "Android Bluetooth service is unavailable", 0);
            return null;
        }
        Server opened = new Server(id(args, "peripheral"), args.requireField("local_services"), result);
        opened.gatt = manager.openGattServer(context, opened);
        if (opened.gatt == null) {
            fail(result, "unavailable", "Android could not open a GATT server", 0);
            return null;
        }
        server = opened;
        try {
            opened.addNext();
        } catch (RuntimeException error) {
            closeServer(opened.token);
            throw error;
        }
        return () -> closeServer(opened.token);
    }

    void closeServer(long token) {
        if (server == null || server.token != token)
            return;
        Server stopped = server;
        server = null;
        stopped.close();
    }

    void dispose() {
        if (advertisement != null)
            stopAdvertising(advertisement.token);
        if (server != null)
            closeServer(server.token);
    }

    void respond(PlatformPayload args) {
        if (server == null || server.token != id(args, "peripheral"))
            return;
        server.respond(id(args, "request"), (int) id(args, "status"), args.requireField("value").requireBytes());
    }

    HuxerUIPlatformChannel.Cancellation notify(PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        Server opened = server;
        if (opened == null || opened.token != id(args, "peripheral")) {
            fail(result, "disconnected", "The Android GATT server is closed", 0);
            return null;
        }
        Notification notification = opened.notifyValue(id(args, "attribute"), args.requireField("value").requireBytes(),
                result);
        return notification == null ? null : () -> {
            if (server == opened && opened.notification == notification) {
                closeServer(opened.token);
                emitter.emit("peripheral_error", opened.token,
                        failure("disconnected", "The Android notification was interrupted", 0));
            }
        };
    }

    private static final class Attribute {
        final long id;
        final BluetoothGattCharacteristic characteristic;
        final Set<BluetoothDevice> subscribers = new HashSet<>();

        Attribute(long id, BluetoothGattCharacteristic characteristic) {
            this.id = id;
            this.characteristic = characteristic;
        }
    }

    private static final class Notification {
        final Attribute attribute;
        final byte[] value;
        final ArrayDeque<BluetoothDevice> recipients;
        final HuxerUIPlatformChannel.Result result;
        BluetoothDevice sendingTo;

        Notification(Attribute attribute, byte[] value, HuxerUIPlatformChannel.Result result) {
            this.attribute = attribute;
            this.value = value;
            this.recipients = new ArrayDeque<>(attribute.subscribers);
            this.result = result;
        }
    }

    private static final class AttRequest {
        final BluetoothDevice peer;
        final int requestId;
        final int offset;
        final Runnable deadline;

        AttRequest(BluetoothDevice peer, int requestId, int offset, Runnable deadline) {
            this.peer = peer;
            this.requestId = requestId;
            this.offset = offset;
            this.deadline = deadline;
        }
    }

    private final class Server extends BluetoothGattServerCallback {
        final long token;
        final ArrayDeque<BluetoothGattService> services = new ArrayDeque<>();
        final Map<BluetoothGattCharacteristic, Attribute> byObject = new IdentityHashMap<>();
        final Map<Long, Attribute> byId = new HashMap<>();
        final Map<BluetoothDevice, Integer> mtu = new HashMap<>();
        final Map<Long, AttRequest> pending = new HashMap<>();
        HuxerUIPlatformChannel.Result opening;
        BluetoothGattServer gatt;
        Notification notification;
        long nextRequest = 1;

        Server(long token, PlatformPayload definition, HuxerUIPlatformChannel.Result opening) {
            this.token = token;
            this.opening = opening;
            for (PlatformPayload service : definition.elements()) {
                BluetoothGattService entry = new BluetoothGattService(
                        UUID.fromString(service.requireField("uuid").requireString()), BluetoothGattService.SERVICE_TYPE_PRIMARY);
                for (PlatformPayload characteristic : service.requireField("characteristics").elements()) {
                    int properties = (int) id(characteristic, "properties");
                    int permissions = 0;
                    if ((properties & BluetoothGattCharacteristic.PROPERTY_READ) != 0)
                        permissions |= BluetoothGattCharacteristic.PERMISSION_READ;
                    if ((properties & BluetoothGattCharacteristic.PROPERTY_WRITE) != 0)
                        permissions |= BluetoothGattCharacteristic.PERMISSION_WRITE;
                    BluetoothGattCharacteristic item = new BluetoothGattCharacteristic(
                            UUID.fromString(characteristic.requireField("uuid").requireString()), properties, permissions);
                    if ((properties & BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0)
                        item.addDescriptor(new BluetoothGattDescriptor(CLIENT_CONFIGURATION,
                                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE));
                    Attribute attribute = new Attribute(id(characteristic, "id"), item);
                    byId.put(attribute.id, attribute);
                    byObject.put(item, attribute);
                    entry.addCharacteristic(item);
                }
                services.add(entry);
            }
        }

        void addNext() {
            if (services.isEmpty()) {
                HuxerUIPlatformChannel.Result ready = opening;
                opening = null;
                ready.complete(EMPTY);
            } else if (!gatt.addService(services.peek())) {
                fail(opening, "platform", "Android rejected a local GATT service", 0);
                closeServer(token);
            }
        }

        @Override
        public void onServiceAdded(int status, BluetoothGattService service) {
            handler.post(() -> {
                if (server != this || opening == null)
                    return;
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail(opening, "platform", "Android could not register a local GATT service", status);
                    closeServer(token);
                    return;
                }
                services.remove();
                try {
                    addNext();
                } catch (SecurityException error) {
                    fail(opening, "permission", "Android Bluetooth access was revoked", 0);
                    closeServer(token);
                }
            });
        }

        @Override
        public void onCharacteristicReadRequest(BluetoothDevice peer, int requestId, int offset,
                                                BluetoothGattCharacteristic characteristic) {
            handler.post(() -> request(peer, requestId, offset, characteristic, false, new byte[0]));
        }

        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice peer, int requestId,
                                                 BluetoothGattCharacteristic characteristic, boolean prepared,
                                                 boolean responseNeeded, int offset, byte[] value) {
            if (!responseNeeded)
                return;
            byte[] owned = copy(value);
            handler.post(() -> {
                if (server != this)
                    return;
                if (prepared)
                    reply(peer, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, offset, null);
                else
                    request(peer, requestId, offset, characteristic, true, owned);
            });
        }

        @Override
        public void onExecuteWrite(BluetoothDevice peer, int requestId, boolean execute) {
            handler.post(() -> {
                if (server == this)
                    reply(peer, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, 0, null);
            });
        }

        private void request(BluetoothDevice peer, int requestId, int offset, BluetoothGattCharacteristic characteristic,
                             boolean write, byte[] bytes) {
            if (server != this)
                return;
            Attribute attribute = byObject.get(characteristic);
            if (attribute == null || pending.size() >= 64) {
                reply(peer, requestId, BluetoothGatt.GATT_FAILURE, offset, null);
                return;
            }
            long request = nextRequest++;
            Runnable deadline = () -> respond(request, BluetoothGatt.GATT_FAILURE, null);
            pending.put(request, new AttRequest(peer, requestId, offset, deadline));
            handler.postDelayed(deadline, 5000);
            emitter.emit(write ? "server_write" : "server_read", token, object("request", number(request),
                    "attribute", number(attribute.id), "offset", number(offset),
                    "limit", number(mtu.getOrDefault(peer, 23) - 1), "bytes", PlatformPayload.bytes(bytes)));
        }

        void respond(long id, int status, byte[] value) {
            AttRequest request = pending.remove(id);
            if (request == null)
                return;
            handler.removeCallbacks(request.deadline);
            reply(request.peer, request.requestId, status, request.offset, value);
        }

        private void reply(BluetoothDevice peer, int request, int status, int offset, byte[] value) {
            try {
                gatt.sendResponse(peer, request, status, offset, value);
            } catch (SecurityException error) {
                emitter.emit("peripheral_error", token, failure("permission", "Android Bluetooth access was revoked", 0));
            }
        }

        @Override
        public void onDescriptorReadRequest(BluetoothDevice peer, int requestId, int offset,
                                            BluetoothGattDescriptor descriptor) {
            handler.post(() -> {
                if (server != this)
                    return;
                Attribute attribute = byObject.get(descriptor.getCharacteristic());
                if (attribute == null || !CLIENT_CONFIGURATION.equals(descriptor.getUuid())) {
                    reply(peer, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, offset, null);
                } else if (offset < 0 || offset > 2) {
                    reply(peer, requestId, BluetoothGatt.GATT_INVALID_OFFSET, offset, null);
                } else {
                    byte[] value = attribute.subscribers.contains(peer) ? BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            : BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE;
                    reply(peer, requestId, BluetoothGatt.GATT_SUCCESS, offset, Arrays.copyOfRange(value, offset, 2));
                }
            });
        }

        @Override
        public void onDescriptorWriteRequest(BluetoothDevice peer, int requestId, BluetoothGattDescriptor descriptor,
                                            boolean prepared, boolean responseNeeded, int offset, byte[] value) {
            if (!responseNeeded)
                return;
            byte[] owned = copy(value);
            handler.post(() -> {
                if (server != this)
                    return;
                Attribute attribute = byObject.get(descriptor.getCharacteristic());
                boolean enable = Arrays.equals(owned, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                int status = BluetoothGatt.GATT_SUCCESS;
                if (attribute == null || prepared || !CLIENT_CONFIGURATION.equals(descriptor.getUuid()))
                    status = BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED;
                else if (offset != 0)
                    status = BluetoothGatt.GATT_INVALID_OFFSET;
                else if (owned.length != 2)
                    status = BluetoothGatt.GATT_INVALID_ATTRIBUTE_LENGTH;
                else if (!enable && !Arrays.equals(owned, BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE))
                    status = BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED;
                if (status == BluetoothGatt.GATT_SUCCESS)
                    subscribe(attribute, peer, enable);
                reply(peer, requestId, status, offset, null);
            });
        }

        private void subscribe(Attribute attribute, BluetoothDevice peer, boolean enable) {
            boolean old = !attribute.subscribers.isEmpty();
            if (enable)
                attribute.subscribers.add(peer);
            else
                attribute.subscribers.remove(peer);
            boolean active = !attribute.subscribers.isEmpty();
            if (active != old)
                emitter.emit("server_subscription", token, object("attribute", number(attribute.id),
                        "subscribed", PlatformPayload.booleanValue(active)));
        }

        @Override
        public void onConnectionStateChange(BluetoothDevice peer, int status, int state) {
            if (state != BluetoothProfile.STATE_DISCONNECTED)
                return;
            handler.post(() -> {
                if (server != this)
                    return;
                mtu.remove(peer);
                for (Attribute attribute : byId.values())
                    subscribe(attribute, peer, false);
                pending.values().removeIf(request -> {
                    if (!peer.equals(request.peer))
                        return false;
                    handler.removeCallbacks(request.deadline);
                    return true;
                });
            });
        }

        @Override
        public void onMtuChanged(BluetoothDevice peer, int mtuSize) {
            handler.post(() -> {
                if (server == this && mtuSize >= 23)
                    mtu.put(peer, mtuSize);
            });
        }

        Notification notifyValue(long id, byte[] value, HuxerUIPlatformChannel.Result result) {
            if (notification != null) {
                fail(result, "busy", "An Android notification is already in progress", 0);
                return null;
            }
            Attribute attribute = byId.get(id);
            if (attribute == null) {
                fail(result, "property", "The local notification characteristic is unavailable", 0);
                return null;
            }
            for (BluetoothDevice peer : attribute.subscribers) {
                if (value.length > mtu.getOrDefault(peer, 23) - 3) {
                    fail(result, "length", "The notification exceeds a subscribed peer's update size", 0);
                    return null;
                }
            }
            Notification sending = new Notification(attribute, value, result);
            notification = sending;
            sendNext();
            return notification == sending ? sending : null;
        }

        private void sendNext() {
            Notification sending = notification;
            while (!sending.recipients.isEmpty()) {
                BluetoothDevice peer = sending.recipients.remove();
                if (!sending.attribute.subscribers.contains(peer))
                    continue;
                sending.sendingTo = peer;
                BluetoothGattCharacteristic characteristic = sending.attribute.characteristic;
                try {
                    boolean accepted;
                    if (Build.VERSION.SDK_INT >= 33) {
                        accepted = gatt.notifyCharacteristicChanged(peer, characteristic, false, sending.value)
                                == BluetoothStatusCodes.SUCCESS;
                    } else {
                        accepted = characteristic.setValue(sending.value)
                                && gatt.notifyCharacteristicChanged(peer, characteristic, false);
                    }
                    if (accepted)
                        return;
                    fail(sending.result, "platform", "Android rejected the notification", 0);
                } catch (SecurityException error) {
                    fail(sending.result, "permission", "Android Bluetooth access was revoked", 0);
                }
                notification = null;
                return;
            }
            notification = null;
            sending.result.complete(EMPTY);
        }

        @Override
        public void onNotificationSent(BluetoothDevice peer, int status) {
            handler.post(() -> {
                Notification sending = notification;
                if (server != this || sending == null || !peer.equals(sending.sendingTo))
                    return;
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    sendNext();
                } else {
                    notification = null;
                    fail(sending.result, "platform", "Android did not send the notification", status);
                }
            });
        }

        void close() {
            for (AttRequest request : pending.values())
                handler.removeCallbacks(request.deadline);
            pending.clear();
            notification = null;
            opening = null;
            try {
                gatt.close();
            } catch (SecurityException ignored) {
                // The platform already removed GATT access.
            }
        }
    }
}
