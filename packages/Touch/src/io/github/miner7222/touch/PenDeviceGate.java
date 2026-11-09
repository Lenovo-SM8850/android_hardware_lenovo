/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touch;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.util.Log;
import android.view.InputDevice;

import java.util.Arrays;
import java.util.List;

/**
 * Turns the touch panel's own pen input device off while a game session blocks pen input.
 * High reporting, when the game keeps it on, also selects the firmware's pen-blocking mode;
 * disabling the matching input device blocks the pen in either mode and also prevents queued
 * panel pen events from reaching the game.
 * Finger touch is a separate device and stays on. A device added again while blocked, for
 * example after the panel resumes, is disabled as soon as it appears.
 */
final class PenDeviceGate implements InputManager.InputDeviceListener {
    private static final String TAG = "LenovoPenGate";

    private final InputManager mInputManager;
    private final List<String> mNames;
    private boolean mBlocked;

    PenDeviceGate(Context context, Handler handler) {
        mInputManager = context.getSystemService(InputManager.class);
        mNames = Arrays.asList(context.getResources().getStringArray(R.array.config_panelPenDevices));
        if (mInputManager != null && !mNames.isEmpty()) {
            mInputManager.registerInputDeviceListener(this, handler);
        }
    }

    /** Blocks or restores the panel pen. Called with false on start to undo a crashed block. */
    void setBlocked(boolean blocked) {
        mBlocked = blocked;
        if (mInputManager == null) return;
        for (int id : mInputManager.getInputDeviceIds()) apply(id);
    }

    private void apply(int id) {
        InputDevice device = mInputManager.getInputDevice(id);
        if (device == null || device.isExternal() || !mNames.contains(device.getName())) return;
        try {
            if (mBlocked) {
                mInputManager.disableInputDevice(id);
            } else {
                mInputManager.enableInputDevice(id);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Cannot change pen device " + device.getName(), e);
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        if (mBlocked) apply(deviceId);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
    }
}
