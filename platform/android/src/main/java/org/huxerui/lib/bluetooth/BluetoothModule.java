package org.huxerui.lib.bluetooth;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

@SuppressWarnings("deprecation")
public final class BluetoothModule implements HuxerUIPlatformModule {
    static final UUID CLIENT_CONFIGURATION =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    static final PlatformPayload EMPTY = PlatformPayload.nullValue();

    private final Context context;
    private final BluetoothAdapter adapter;
    private final Peripheral peripheral;
    private final HuxerUIPlatformChannel.Events events;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<String, BluetoothDevice> devices = new HashMap<>();
    private final Map<String, HuxerUIPlatformChannel.Result> pairing = new HashMap<>();
    private final Map<Long, Link> links = new HashMap<>();
    private BluetoothLeScanner scanner;
    private ScanCallback scanCallback;
    private long scanToken;
    private long nextAttribute = 1;

    public static final class Factory implements HuxerUIPlatformModule.Factory {
        @Override
        public HuxerUIPlatformModule create(
                Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events) {
            options.requireNull();
            return new BluetoothModule(context, events);
        }
    }

    private BluetoothModule(Context context, HuxerUIPlatformChannel.Events events) {
        this.context = context.getApplicationContext();
        this.events = events;
        BluetoothManager manager = this.context.getSystemService(BluetoothManager.class);
        adapter = manager == null ? null : manager.getAdapter();
        peripheral = new Peripheral(this.context, adapter, handler, this::emit);
        IntentFilter filter = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        filter.addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            this.context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            this.context.registerReceiver(receiver, filter);
        }
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ignored, Intent intent) {
            if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(intent.getAction())) {
                emit("status", 0, statusPayload());
                return;
            }
            BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR);
            if (device == null || state == BluetoothDevice.BOND_BONDING) {
                return;
            }
            HuxerUIPlatformChannel.Result pending = pairing.remove(device.getAddress());
            if (pending == null) {
                return;
            }
            if (state == BluetoothDevice.BOND_BONDED) {
                pending.complete(EMPTY);
            } else {
                fail(pending, "pairing", "Android did not establish a Bluetooth bond", state);
            }
        }
    };

    private int status() {
        if (adapter == null || !context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) {
            return 1;
        }
        if (Build.VERSION.SDK_INT >= 31
                && context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
            return 0;
        }
        int state = adapter.getState();
        return state == BluetoothAdapter.STATE_ON ? 3 : state == BluetoothAdapter.STATE_OFF ? 2 : 0;
    }

    private PlatformPayload statusPayload() {
        int current = status();
        return object("status", number(current), "peripheral", peripheral.capabilities(current != 1));
    }

    @Override
    public HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        try {
            switch (method) {
                case "status":
                    result.complete(statusPayload());
                    return null;
                case "stop_advertising":
                    peripheral.stopAdvertising(args.requireInt64());
                    result.complete(EMPTY);
                    return null;
                case "close_server":
                    peripheral.closeServer(args.requireInt64());
                    result.complete(EMPTY);
                    return null;
                case "server_response":
                    peripheral.respond(args);
                    result.complete(EMPTY);
                    return null;
                case "stop_scan":
                    stopScan(args.requireInt64());
                    result.complete(EMPTY);
                    return null;
                case "disconnect":
                    closeLink(args.requireInt64());
                    result.complete(EMPTY);
                    return null;
                default:
                    break;
            }

            int current = status();
            if (current == 1 || current == 2 && !method.equals("restore") && !method.equals("bond")) {
                fail(result, current == 1 ? "unavailable" : "off", "Android Bluetooth adapter is unavailable", current);
                return null;
            }
            switch (method) {
                case "advertise":
                    return peripheral.startAdvertising(args, result);
                case "open_server":
                    return peripheral.openServer(args, result);
                case "send_notification":
                    return peripheral.notify(args, result);
                case "scan":
                    startScan(args, result);
                    return null;
                case "restore":
                case "bond":
                case "pair":
                case "connect":
                    return deviceOperation(method, args, result);
                case "discover":
                case "read":
                case "write":
                case "notify":
                    Link link = links.get(id(args, "link"));
                    if (link == null) {
                        fail(result, "disconnected", "Android GATT session is closed", 0);
                    } else {
                        link.invoke(method, args, result);
                    }
                    return null;
                default:
                    fail(result, "unsupported", "Android does not support this Bluetooth operation", 0);
                    return null;
            }
        } catch (SecurityException error) {
            fail(result, "permission", "Android Bluetooth permission has not been granted", 0);
        } catch (RuntimeException error) {
            fail(result, "platform", "Android Bluetooth request failed: " + error.getClass().getSimpleName(), 0);
        }
        return null;
    }

    private HuxerUIPlatformChannel.Cancellation deviceOperation(
            String method, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        String address = args.requireField("device").requireString();
        BluetoothDevice device = devices.get(address);
        if (device == null && method.equals("restore")) {
            for (BluetoothDevice bonded : adapter.getBondedDevices()) {
                if (address.equals(bonded.getAddress())) {
                    device = bonded;
                    devices.put(address, device);
                    break;
                }
            }
        }
        if (device == null) {
            fail(result, "device", "Android no longer recognizes this Bluetooth device", 0);
            return null;
        }
        switch (method) {
            case "restore":
                result.complete(deviceInfo(device));
                return null;
            case "bond":
                int state = device.getBondState();
                result.complete(number(state == BluetoothDevice.BOND_BONDED ? 3
                        : state == BluetoothDevice.BOND_BONDING ? 2 : state == BluetoothDevice.BOND_NONE ? 1 : 0));
                return null;
            case "pair":
                return pair(device, result);
            case "connect":
                long id = id(args, "link");
                Link link = new Link(id, result);
                links.put(id, link);
                try {
                    link.gatt = device.connectGatt(context, false, link, BluetoothDevice.TRANSPORT_LE);
                    if (link.gatt == null) {
                        links.remove(id);
                        fail(result, "device", "Android could not open the GATT connection", 0);
                    }
                } catch (RuntimeException error) {
                    links.remove(id);
                    throw error;
                }
                return null;
            default:
                return null;
        }
    }

    private HuxerUIPlatformChannel.Cancellation pair(BluetoothDevice device, HuxerUIPlatformChannel.Result result) {
        String address = device.getAddress();
        int state = device.getBondState();
        if (state == BluetoothDevice.BOND_BONDED) {
            result.complete(EMPTY);
            return null;
        }
        if (pairing.containsKey(address)) {
            fail(result, "busy", "A bond request for this device is already active", 0);
            return null;
        }
        pairing.put(address, result);
        try {
            if (state != BluetoothDevice.BOND_BONDING && !device.createBond()) {
                pairing.remove(address);
                fail(result, "pairing", "Android rejected the Bluetooth bond request", 0);
            }
        } catch (RuntimeException error) {
            pairing.remove(address);
            throw error;
        }
        // Android can continue showing the system pairing prompt after cancellation.
        return null;
    }

    private void startScan(PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        BluetoothLeScanner next = adapter.getBluetoothLeScanner();
        if (next == null) {
            fail(result, "off", "Android BLE scanner is unavailable", 0);
            return;
        }
        long token = id(args, "scan");
        List<ScanFilter> filters = new ArrayList<>();
        for (PlatformPayload uuid : args.requireField("services").elements()) {
            filters.add(new ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString(uuid.requireString())).build());
        }
        ScanCallback callback = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, ScanResult value) {
                handler.post(() -> reportScan(token, value));
            }

            @Override
            public void onBatchScanResults(List<ScanResult> results) {
                handler.post(() -> {
                    for (ScanResult value : results) {
                        reportScan(token, value);
                    }
                });
            }

            @Override
            public void onScanFailed(int errorCode) {
                handler.post(() -> {
                    if (scanToken == token) {
                        emit("scan_error", token, failure("platform", "Android BLE scan failed", errorCode));
                    }
                });
            }
        };
        scanner = next;
        scanToken = token;
        scanCallback = callback;
        try {
            scanner.startScan(filters, new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .build(), callback);
            result.complete(EMPTY);
        } catch (RuntimeException error) {
            stopScan(token);
            throw error;
        }
    }

    private void reportScan(long token, ScanResult value) {
        if (token != scanToken) {
            return;
        }
        BluetoothDevice device = value.getDevice();
        devices.put(device.getAddress(), device);
        ScanRecord record = value.getScanRecord();
        List<PlatformPayload> services = new ArrayList<>();
        List<PlatformPayload> manufacturer = new ArrayList<>();
        Map<String, PlatformPayload> serviceData = new HashMap<>();
        if (record != null) {
            if (record.getServiceUuids() != null) {
                for (ParcelUuid uuid : record.getServiceUuids()) {
                    services.add(PlatformPayload.string(uuid.toString()));
                }
            }
            if (record.getManufacturerSpecificData() != null) {
                for (int i = 0; i < record.getManufacturerSpecificData().size(); ++i) {
                    int id = record.getManufacturerSpecificData().keyAt(i);
                    manufacturer.add(object("id", number(id),
                            "value", PlatformPayload.bytes(record.getManufacturerSpecificData().valueAt(i))));
                }
            }
            if (record.getServiceData() != null) {
                for (Map.Entry<ParcelUuid, byte[]> item : record.getServiceData().entrySet()) {
                    serviceData.put(item.getKey().toString(), PlatformPayload.bytes(item.getValue()));
                }
            }
        }
        emit("scan", token, object("device", deviceInfo(device),
                "local_name", text(record == null ? null : record.getDeviceName()),
                "services", PlatformPayload.list(services), "manufacturer", PlatformPayload.list(manufacturer),
                "service_data", PlatformPayload.object(serviceData), "rssi", number(value.getRssi()),
                "connectable", Build.VERSION.SDK_INT >= 26 ? PlatformPayload.booleanValue(value.isConnectable()) : EMPTY));
    }

    private void stopScan(long token) {
        if (scanToken != token || scanCallback == null) {
            return;
        }
        ScanCallback previous = scanCallback;
        BluetoothLeScanner owner = scanner;
        scanToken = 0;
        scanCallback = null;
        scanner = null;
        try {
            owner.stopScan(previous);
        } catch (SecurityException ignored) {
            // The platform already removed scan access.
        }
    }

    private PlatformPayload deviceInfo(BluetoothDevice device) {
        String name = null;
        try {
            name = device.getName();
        } catch (SecurityException ignored) {
            // Discovery can report the advertising name with scan access alone.
        }
        return object("id", text(device.getAddress()), "name", text(name));
    }

    private void closeLink(long id) {
        Link link = links.remove(id);
        if (link != null) {
            link.close();
        }
    }

    @Override
    public void dispose() {
        context.unregisterReceiver(receiver);
        stopScan(scanToken);
        peripheral.dispose();
        for (long id : new ArrayList<>(links.keySet())) {
            closeLink(id);
        }
        pairing.clear();
        devices.clear();
    }

    private final class Link extends BluetoothGattCallback {
        private final long id;
        private final Map<Long, Object> attributes = new HashMap<>();
        private final Set<BluetoothGattCharacteristic> notifying = new HashSet<>();
        private final ArrayDeque<BluetoothGattCharacteristic> disabling = new ArrayDeque<>();
        private BluetoothGatt gatt;
        private HuxerUIPlatformChannel.Result pending;
        private String method = "connect";
        private Object attribute;
        private boolean notificationEnabled;
        private int mtu = 23;

        Link(long id, HuxerUIPlatformChannel.Result connect) {
            this.id = id;
            this.pending = connect;
        }

        private boolean current(BluetoothGatt source) {
            return links.get(id) == this && gatt == source;
        }

        private PlatformPayload limits() {
            return object("response", number(512), "command", number(Math.min(512, Math.max(0, mtu - 3))));
        }

        private void invoke(String operation, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
            if (pending != null) {
                fail(result, "busy", "An Android GATT request is still active", 0);
                return;
            }
            method = operation;
            pending = result;
            attribute = null;
            try {
                if (operation.equals("discover")) {
                    disabling.addAll(notifying);
                    continueDiscovery();
                    return;
                }
                long token = id(args, "attribute");
                attribute = attributes.get(token);
                if (attribute == null) {
                    finishError("stale", "The requested Android GATT attribute is stale", 0);
                    return;
                }
                switch (operation) {
                    case "read":
                        startRead();
                        break;
                    case "write":
                        startWrite(args);
                        break;
                    case "notify":
                        notificationEnabled = args.requireField("enabled").requireBoolean();
                        if (!setNotifications((BluetoothGattCharacteristic) attribute, notificationEnabled)) {
                            finishError("platform", "Android could not configure notifications", 0);
                        }
                        break;
                    default:
                        finishError("unsupported", "Unsupported Android GATT request", 0);
                }
            } catch (SecurityException error) {
                finishError("permission", "Android Bluetooth permission has not been granted", 0);
            } catch (RuntimeException error) {
                finishError("platform", "Android GATT request failed: " + error.getClass().getSimpleName(), 0);
            }
        }

        private void startRead() {
            boolean started = attribute instanceof BluetoothGattDescriptor
                    ? gatt.readDescriptor((BluetoothGattDescriptor) attribute)
                    : gatt.readCharacteristic((BluetoothGattCharacteristic) attribute);
            if (!started) {
                finishError("platform", "Android did not start the GATT read", 0);
            }
        }

        private void startWrite(PlatformPayload args) {
            byte[] bytes = args.requireField("value").requireBytes();
            if (attribute instanceof BluetoothGattDescriptor) {
                if (!writeDescriptor((BluetoothGattDescriptor) attribute, bytes)) {
                    finishError("platform", "Android did not start the GATT descriptor write", 0);
                }
                return;
            }
            BluetoothGattCharacteristic characteristic = (BluetoothGattCharacteristic) attribute;
            int type = args.requireField("response").requireBoolean()
                    ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE;
            boolean started;
            if (Build.VERSION.SDK_INT >= 33) {
                int status = gatt.writeCharacteristic(characteristic, bytes, type);
                started = status == BluetoothStatusCodes.SUCCESS;
                if (!started) {
                    finishError("platform", "Android did not start the GATT characteristic write", status);
                }
            } else {
                characteristic.setWriteType(type);
                started = characteristic.setValue(bytes) && gatt.writeCharacteristic(characteristic);
                if (!started) {
                    finishError("platform", "Android did not start the GATT characteristic write", 0);
                }
            }
        }

        private boolean writeDescriptor(BluetoothGattDescriptor descriptor, byte[] value) {
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS;
            }
            return descriptor.setValue(value) && gatt.writeDescriptor(descriptor);
        }

        private boolean setNotifications(BluetoothGattCharacteristic characteristic, boolean enabled) {
            BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CLIENT_CONFIGURATION);
            if (descriptor == null || !gatt.setCharacteristicNotification(characteristic, enabled)) {
                return false;
            }
            byte[] value = !enabled ? BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
                    : (characteristic.getProperties() & BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
                        ? BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        : BluetoothGattDescriptor.ENABLE_INDICATION_VALUE;
            if (writeDescriptor(descriptor, value)) {
                return true;
            }
            gatt.setCharacteristicNotification(characteristic, notifying.contains(characteristic));
            return false;
        }

        private void continueDiscovery() {
            if (!disabling.isEmpty()) {
                BluetoothGattCharacteristic first = disabling.peek();
                if (!setNotifications(first, false)) {
                    failDiscovery(0);
                }
            } else if (!gatt.discoverServices()) {
                failDiscovery(0);
            }
        }

        private void failDiscovery(int status) {
            disabling.clear();
            finishError(gattError(status), "Android GATT service discovery failed", status);
            emit("services", id, EMPTY);
        }

        private PlatformPayload discover(BluetoothGatt source) {
            attributes.clear();
            List<PlatformPayload> services = new ArrayList<>();
            for (BluetoothGattService service : source.getServices()) {
                List<PlatformPayload> characteristics = new ArrayList<>();
                for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                    long token = nextAttribute++;
                    attributes.put(token, characteristic);
                    List<PlatformPayload> descriptors = new ArrayList<>();
                    for (BluetoothGattDescriptor descriptor : characteristic.getDescriptors()) {
                        long descriptorId = nextAttribute++;
                        attributes.put(descriptorId, descriptor);
                        descriptors.add(object("id", number(descriptorId), "uuid", text(descriptor.getUuid().toString())));
                    }
                    characteristics.add(object("id", number(token), "uuid", text(characteristic.getUuid().toString()),
                            "properties", number(characteristic.getProperties()),
                            "descriptors", PlatformPayload.list(descriptors)));
                }
                services.add(object("uuid", text(service.getUuid().toString()),
                        "characteristics", PlatformPayload.list(characteristics)));
            }
            return PlatformPayload.list(services);
        }

        @Override
        public void onConnectionStateChange(BluetoothGatt source, int status, int newState) {
            handler.post(() -> {
                if (!current(source)) {
                    return;
                }
                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    if (method.equals("connect")) {
                        finish(limits());
                    }
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
                    finishError(method.equals("connect") ? "device" : "disconnected",
                            "Android GATT connection ended", status);
                    emit("link_lost", id, failure("disconnected", "Android GATT connection ended", status));
                    closeLink(id);
                }
            });
        }

        @Override
        public void onMtuChanged(BluetoothGatt source, int value, int status) {
            handler.post(() -> {
                if (current(source) && status == BluetoothGatt.GATT_SUCCESS) {
                    mtu = value;
                    emit("limits", id, limits());
                }
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt source, int status) {
            handler.post(() -> {
                if (current(source) && method.equals("discover") && pending != null) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        finish(discover(source));
                    } else {
                        failDiscovery(status);
                    }
                }
            });
        }

        @Override
        public void onServiceChanged(BluetoothGatt source) {
            handler.post(() -> {
                if (current(source)) {
                    emit("services", id, EMPTY);
                }
            });
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt source, BluetoothGattCharacteristic item, int status) {
            if (Build.VERSION.SDK_INT < 33) {
                onCharacteristicRead(source, item, copy(item.getValue()), status);
            }
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt source, BluetoothGattCharacteristic item, byte[] value, int status) {
            byte[] data = copy(value);
            handler.post(() -> attributeResult(source, item, "read", PlatformPayload.bytes(data), status));
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt source, BluetoothGattCharacteristic item, int status) {
            handler.post(() -> attributeResult(source, item, "write", EMPTY, status));
        }

        @Override
        public void onDescriptorRead(BluetoothGatt source, BluetoothGattDescriptor item, int status) {
            if (Build.VERSION.SDK_INT < 33) {
                onDescriptorRead(source, item, status, copy(item.getValue()));
            }
        }

        @Override
        public void onDescriptorRead(BluetoothGatt source, BluetoothGattDescriptor item, int status, byte[] value) {
            byte[] data = copy(value);
            handler.post(() -> attributeResult(source, item, "read", PlatformPayload.bytes(data), status));
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt source, BluetoothGattDescriptor item, int status) {
            handler.post(() -> {
                if (!current(source) || pending == null) {
                    return;
                }
                if (method.equals("notify") && item.getCharacteristic() == attribute
                        && item.getUuid().equals(CLIENT_CONFIGURATION)) {
                    BluetoothGattCharacteristic characteristic = (BluetoothGattCharacteristic) attribute;
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        if (notificationEnabled) {
                            notifying.add(characteristic);
                        } else {
                            notifying.remove(characteristic);
                        }
                    } else {
                        try {
                            gatt.setCharacteristicNotification(characteristic, notifying.contains(characteristic));
                        } catch (SecurityException ignored) {
                            // Permission loss is reported with the GATT operation result.
                        }
                    }
                    statusResult(EMPTY, status);
                } else if (method.equals("discover") && disabling.peek() == item.getCharacteristic()
                        && item.getUuid().equals(CLIENT_CONFIGURATION)) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        notifying.remove(disabling.remove());
                        try {
                            continueDiscovery();
                        } catch (RuntimeException error) {
                            failDiscovery(0);
                        }
                    } else {
                        failDiscovery(status);
                    }
                } else {
                    attributeResult(source, item, "write", EMPTY, status);
                }
            });
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic item) {
            if (Build.VERSION.SDK_INT < 33) {
                onCharacteristicChanged(source, item, copy(item.getValue()));
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic item, byte[] value) {
            byte[] data = copy(value);
            handler.post(() -> {
                if (!current(source)) {
                    return;
                }
                for (Map.Entry<Long, Object> entry : attributes.entrySet()) {
                    if (entry.getValue() == item) {
                        emit("value", id, object("attribute", number(entry.getKey()), "bytes", PlatformPayload.bytes(data)));
                        break;
                    }
                }
            });
        }

        private void attributeResult(BluetoothGatt source, Object item, String operation,
                                     PlatformPayload value, int status) {
            if (current(source) && pending != null && method.equals(operation) && attribute == item) {
                statusResult(value, status);
            }
        }

        private void statusResult(PlatformPayload value, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                finish(value);
            } else {
                finishError(gattError(status), "Android GATT operation failed", status);
            }
        }

        private void finish(PlatformPayload value) {
            HuxerUIPlatformChannel.Result result = pending;
            pending = null;
            if (result != null) {
                result.complete(value);
            }
        }

        private void finishError(String code, String message, int status) {
            HuxerUIPlatformChannel.Result result = pending;
            pending = null;
            if (result != null) {
                fail(result, code, message, status);
            }
        }

        private void close() {
            pending = null;
            BluetoothGatt owner = gatt;
            gatt = null;
            if (owner != null) {
                try {
                    owner.disconnect();
                    owner.close();
                } catch (SecurityException ignored) {
                    // The platform already removed the application's GATT access.
                }
            }
            attributes.clear();
            notifying.clear();
            disabling.clear();
        }
    }

    private static String gattError(int status) {
        switch (status) {
            case BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION:
            case BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION:
            case 8:
                return "authentication";
            case BluetoothGatt.GATT_INVALID_ATTRIBUTE_LENGTH:
                return "length";
            case BluetoothGatt.GATT_READ_NOT_PERMITTED:
            case BluetoothGatt.GATT_WRITE_NOT_PERMITTED:
                return "property";
            default:
                return "platform";
        }
    }

    static byte[] copy(byte[] value) {
        return value == null ? new byte[0] : value.clone();
    }

    private void emit(String type, long token, PlatformPayload data) {
        events.emit("bluetooth", object("type", text(type), "token", number(token), "data", data));
    }

    static PlatformPayload failure(String code, String message, int status) {
        return object("code", text("bluetooth/" + code), "message", text(message), "status", number(status));
    }

    static void fail(HuxerUIPlatformChannel.Result result, String code, String message, int status) {
        result.fail("bluetooth/" + code, message, number(status));
    }

    static long id(PlatformPayload object, String key) {
        return object.requireField(key).requireInt64();
    }

    static PlatformPayload number(long value) {
        return PlatformPayload.int64(value);
    }

    private static PlatformPayload text(String value) {
        return value == null ? EMPTY : PlatformPayload.string(value);
    }

    static PlatformPayload object(Object... fields) {
        Map<String, PlatformPayload> values = new HashMap<>();
        for (int index = 0; index < fields.length; index += 2) {
            values.put((String) fields[index], (PlatformPayload) fields[index + 1]);
        }
        return PlatformPayload.object(values);
    }
}
