package org.huxerui.lib.bluetooth.preview;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

import org.huxerui.HuxerUIActivity;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.lang.ref.WeakReference;

public final class MainActivity extends HuxerUIActivity {
    private static final int ADVERTISE_PERMISSION_REQUEST = 0x424c;
    private static WeakReference<MainActivity> resumed = new WeakReference<>(null);
    private HuxerUIPlatformChannel.Result permissionResult;
    private Boolean permissionDecision;

    @Override
    protected void onResume() {
        super.onResume();
        resumed = new WeakReference<>(this);
        getWindow().getDecorView().post(this::deliverPermission);
    }

    @Override
    protected void onPause() {
        if (resumed.get() == this)
            resumed.clear();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        finishPermission(false);
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == ADVERTISE_PERMISSION_REQUEST) {
            permissionDecision = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            getWindow().getDecorView().post(this::deliverPermission);
        } else {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        }
    }

    private void deliverPermission() {
        if (resumed.get() == this && permissionDecision != null)
            finishPermission(permissionDecision);
    }

    private void finishPermission(boolean granted) {
        HuxerUIPlatformChannel.Result pending = permissionResult;
        permissionResult = null;
        permissionDecision = null;
        if (pending != null)
            pending.complete(PlatformPayload.booleanValue(granted));
    }

    public static final class AdvertisePermissionFactory implements HuxerUIPlatformModule.Factory {
        @Override
        public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                             HuxerUIPlatformChannel.Events events) {
            return new HuxerUIPlatformModule() {
                @Override
                public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload args,
                                                                 HuxerUIPlatformChannel.Result result) {
                    if (!"request".equals(method)) {
                        result.fail("bluetooth/unsupported", "Unknown Preview permission operation",
                                PlatformPayload.nullValue());
                        return null;
                    }
                    if (Build.VERSION.SDK_INT < 31 || context.checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE)
                            == PackageManager.PERMISSION_GRANTED) {
                        result.complete(PlatformPayload.booleanValue(true));
                        return null;
                    }
                    MainActivity activity = resumed.get();
                    if (activity == null || activity.permissionResult != null) {
                        result.complete(PlatformPayload.booleanValue(false));
                        return null;
                    }
                    activity.permissionResult = result;
                    try {
                        activity.requestPermissions(new String[]{Manifest.permission.BLUETOOTH_ADVERTISE},
                                ADVERTISE_PERMISSION_REQUEST);
                    } catch (RuntimeException error) {
                        activity.finishPermission(false);
                    }
                    return null;
                }

                @Override
                public void dispose() {}
            };
        }
    }
}
