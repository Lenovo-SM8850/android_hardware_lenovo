/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touchrotation;

import android.app.Application;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Display;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Applies the internal display rotation to Lenovo edge rejection. No settings UI. */
public final class TouchscreenRotationApplication extends Application
        implements DisplayManager.DisplayListener {
    private static final String TAG = "LenovoTouchscreenRotation";
    private static final long RETRY_DELAY_MS = 1000;
    private static final int MAX_RETRIES = 30;

    private DisplayManager mDisplayManager;
    private Handler mHandler;
    // All state below is confined to mHandler.
    private int mAppliedRotation = -1;
    private int mRetryCount;
    private boolean mDisplayOn;
    private final Runnable mApply = this::applyRotation;

    @Override
    public void onCreate() {
        super.onCreate();
        HandlerThread thread = new HandlerThread(TAG);
        thread.start();
        mHandler = new Handler(thread.getLooper());
        mDisplayManager = getSystemService(DisplayManager.class);
        mDisplayManager.registerDisplayListener(this, mHandler);
        mHandler.post(mApply);
    }

    @Override
    public void onDisplayAdded(int displayId) {
        onDisplayChanged(displayId);
    }

    @Override
    public void onDisplayRemoved(int displayId) {
        if (displayId == Display.DEFAULT_DISPLAY) {
            mHandler.removeCallbacks(mApply);
            mAppliedRotation = -1;
            mDisplayOn = false;
        }
    }

    @Override
    public void onDisplayChanged(int displayId) {
        if (displayId == Display.DEFAULT_DISPLAY) {
            mRetryCount = 0;
            applyRotation();
        }
    }

    private void applyRotation() {
        mHandler.removeCallbacks(mApply);
        Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
        if (display == null || display.getState() != Display.STATE_ON) {
            mDisplayOn = false;
            return;
        }
        int rotation = display.getRotation();
        if (rotation < 0 || rotation > 3) {
            Log.e(TAG, "Unexpected rotation: " + rotation);
            return;
        }
        if (mDisplayOn && mAppliedRotation == rotation) {
            return;
        }
        mDisplayOn = true;
        try {
            writeNode("/proc/panel_direction", Integer.toString(rotation));
            // Keep sidebar/taskbar exemptions disabled; the driver buffer is 16 bytes.
            if (!new File("/sys/bus/platform/devices/gt9976n_thp.0").exists()) {
                writeNode("/proc/edge_grid_zone", "0,0,0,0");
            }
            mAppliedRotation = rotation;
            mRetryCount = 0;
        } catch (IOException e) {
            mAppliedRotation = -1;
            if (mRetryCount++ == 0) {
                Log.w(TAG, "Cannot apply Lenovo rotation", e);
            }
            if (mRetryCount <= MAX_RETRIES) {
                mHandler.postDelayed(mApply, RETRY_DELAY_MS);
            } else {
                Log.e(TAG, "Lenovo rotation retries exhausted; awaiting display change");
            }
        }
    }

    private static void writeNode(String path, String value) throws IOException {
        try (FileOutputStream stream = new FileOutputStream(path)) {
            stream.write(value.getBytes(StandardCharsets.US_ASCII));
        }
    }
}
