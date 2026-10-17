/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.content.Context
import android.hardware.input.InputManager
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import com.android.internal.os.DeviceKeyHandler

class PenKeyHandler(private val context: Context) : DeviceKeyHandler {
    override fun handleKeyEvent(event: KeyEvent): KeyEvent? {
        var handled = false
        try {
            if (event.keyCode != KeyEvent.KEYCODE_MACRO_1 ||
                (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP)) {
                return event
            }
            val device = event.device ?: return event
            if (device.vendorId != 0x17ef ||
                (device.productId != 0x617f && device.productId != 0x622e)) {
                return event
            }
            handled = true
            if (event.repeatCount > 0) return null

            val key = KeyEvent(
                event.downTime, event.eventTime, event.action, PEN_DOUBLE_CLICK_KEY,
                event.repeatCount, event.metaState, event.deviceId, event.scanCode,
                event.flags, event.source.takeIf { it != InputDevice.SOURCE_UNKNOWN }
                    ?: InputDevice.SOURCE_KEYBOARD,
            )
            val inputManager = context.getSystemService(InputManager::class.java)
            if (!inputManager.injectInputEvent(key, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC)) {
                Log.w(TAG, "Cannot inject pen double click")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Cannot handle pen double click", e)
        }
        return if (handled) null else event
    }

    companion object {
        private const val TAG = "PenKeyHandler"
        // Apps written for Lenovo Tab Pen Plus and Lenovo Tab Pen Pro 2 expect this key code.
        private const val PEN_DOUBLE_CLICK_KEY = 601
    }
}
