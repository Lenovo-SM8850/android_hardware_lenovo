/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.BatteryState
import android.hardware.input.InputManager
import android.os.Handler
import android.os.SystemClock
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import android.view.InputDevice
import kotlin.math.roundToInt

internal class PenBattery(private val context: Context, private val handler: Handler,
    private val sessions: PenGattSessions) {
    private val inputManager = context.getSystemService(InputManager::class.java)
    private val adapter = context.getSystemService(BluetoothManager::class.java).adapter
    private val preferences = context.createDeviceProtectedStorageContext()
        .getSharedPreferences("pen_battery", Context.MODE_PRIVATE)
    private val pens = mutableMapOf<String, Pen>()
    private val listeners = mutableMapOf<Int, InputManager.InputDeviceBatteryListener>()
    private val batteryStates = mutableMapOf<Int, BatteryState>()

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothDevice.ACTION_BATTERY_LEVEL_CHANGED -> {
                    val device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    pens[device?.address]?.refresh(intent.getIntExtra(
                        BluetoothDevice.EXTRA_BATTERY_LEVEL, BluetoothDevice.BATTERY_LEVEL_UNKNOWN))
                }
                BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    if (adapter?.isEnabled == true) {
                        invalidatePublishedLevels()
                        updateDevices()
                    } else {
                        disconnect()
                    }
                }
            }
        }
    }

    fun start() {
        context.registerReceiverAsUser(receiver, UserHandle.ALL, IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_BATTERY_LEVEL_CHANGED)
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
        }, null, handler, Context.RECEIVER_EXPORTED)
        invalidatePublishedLevels()
    }

    fun updateDevices(devices: List<InputDevice> = inputManager.inputDeviceIds.map {
        inputManager.getInputDevice(it)
    }.filterNotNull()) {
        val connected = mutableMapOf<Int, String>()
        if (adapter?.isEnabled == true) {
            devices.filter { it.isExternal && !it.isVirtual && it.vendorId == 0x17ef &&
                (it.productId == 0x617f || it.productId == 0x622e) }.forEach {
                try {
                    it.bluetoothAddress?.let { address -> connected[it.id] = address }
                } catch (e: RuntimeException) {
                    Log.w(TAG, "Cannot identify Bluetooth pen battery", e)
                }
            }
        }
        for (id in listeners.keys.toList()) {
            if (id !in connected || pens[connected[id]]?.ids?.contains(id) != true) {
                removeListener(id)
            }
        }
        for (address in pens.keys.toList()) {
            if (address !in connected.values) pens.remove(address)?.close()
        }
        connected.entries.groupBy({ it.value }, { it.key }).forEach { (address, ids) ->
            val pro2 = devices.any { it.id in ids && it.vendorId == PenHapticProtocol.VID &&
                it.productId == PenHapticProtocol.PID }
            val pen = pens.getOrPut(address) { Pen(adapter!!.getRemoteDevice(address), pro2) }
            pen.ids = ids
            ids.filter { it !in listeners }.forEach { id ->
                val listener = InputManager.InputDeviceBatteryListener { _, _, state ->
                    if (pens[address] === pen && id in pen.ids) {
                        batteryStates[id] = state
                        pen.refresh()
                    }
                }
                try {
                    inputManager.addInputDeviceBatteryListener(id, { handler.post(it) }, listener)
                    listeners[id] = listener
                } catch (e: RuntimeException) {
                    Log.w(TAG, "Cannot monitor pen battery", e)
                }
            }
            pen.refresh()
        }
    }

    fun onUserSwitched() {
        disconnect()
        updateDevices()
    }

    fun destroy() {
        context.unregisterReceiver(receiver)
        disconnect()
    }

    private fun removeListener(id: Int) {
        batteryStates.remove(id)
        val listener = listeners.remove(id) ?: return
        try {
            inputManager.removeInputDeviceBatteryListener(id, listener)
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot stop pen battery monitor", e)
        }
    }

    private fun disconnect() {
        listeners.keys.toList().forEach { removeListener(it) }
        pens.values.forEach { it.close() }
        pens.clear()
    }

    private fun invalidatePublishedLevels(address: String? = null) {
        if (adapter?.isEnabled != true) return
        preferences.all.keys.filter { it.startsWith("published:") &&
            (address == null || it == "published:$address") }.forEach { key ->
            try {
                val device = adapter.getRemoteDevice(key.removePrefix("published:"))
                // A numeric unknown value is accepted by both Bluetooth and input battery consumers.
                if (device.setMetadata(BluetoothDevice.METADATA_MAIN_BATTERY,
                        BluetoothDevice.BATTERY_LEVEL_UNKNOWN.toString().toByteArray(Charsets.UTF_8))) {
                    preferences.edit().remove(key).commit()
                }
            } catch (e: RuntimeException) {
                Log.w(TAG, "Cannot invalidate pen battery metadata", e)
            }
        }
    }

    private inner class Pen(val device: BluetoothDevice, private val pro2: Boolean) {
        var ids = emptyList<Int>()
        private val user = UserHandle.of(ActivityManager.getCurrentUser())
        private val userContext = context.createContextAsUser(user, 0)
        private val notifications = userContext.getSystemService(NotificationManager::class.java)
        private val connectedAt = SystemClock.uptimeMillis()
        private val session = sessions.get(device)
        private var gattLevel: Int? = null
        private val publishedLevels = mutableSetOf<Int>()
        private var closed = false
        private var gattAttempted = false
        private val fallback = Runnable { refresh() }

        init {
            notifications.createNotificationChannel(NotificationChannel(CHANNEL,
                context.getString(R.string.pen_notification_channel), NotificationManager.IMPORTANCE_HIGH))
        }

        fun refresh(broadcastLevel: Int? = null) {
            if (closed) return
            try {
                val states = ids.mapNotNull {
                    inputManager.getInputDevice(it)?.batteryState?.takeIf { state -> state.isPresent }
                        ?: batteryStates[it]
                }
                val charging = states.any { it.isPresent && it.status == BatteryState.STATUS_CHARGING }
                val stackLevel = device.batteryLevel.takeIf { it in 0..100 }
                    ?: broadcastLevel?.takeIf { it in 0..100 }
                val inputLevel = states.firstNotNullOfOrNull { state ->
                    state.capacity.takeIf { state.isPresent && it.isFinite() && it in 0f..1f }
                        ?.let { (it * 100).roundToInt() }
                        // Input battery state includes our own metadata; it must not mask GATT updates.
                        ?.takeUnless { gattLevel != null && it in publishedLevels }
                }
                val level = stackLevel ?: inputLevel ?: gattLevel
                if (stackLevel != null || inputLevel != null) {
                    closeGatt()
                    gattLevel = null
                    publishedLevels.clear()
                    invalidatePublishedLevels(device.address)
                }
                handler.removeCallbacks(fallback)
                if (stackLevel == null && inputLevel == null && !gattAttempted) {
                    val delay = connectedAt + FALLBACK_DELAY_MS - SystemClock.uptimeMillis()
                    if (delay > 0) handler.postDelayed(fallback, delay) else connectGatt()
                }
                if (charging || (level != null && level >= 30)) cancelNotification()
                if (level != null) updateNotification(level, charging)
            } catch (e: RuntimeException) {
                Log.w(TAG, "Cannot read pen battery", e)
            }
        }

        private fun connectGatt() {
            gattAttempted = true
            session.batteryInterest({ value ->
                handler.post { if (!closed) acceptLevel(value) }
            }, pro2)
        }

        private fun acceptLevel(value: ByteArray) {
            val level = value.singleOrNull()?.toInt()?.and(0xff)?.takeIf { it in 0..100 } ?: return
            gattLevel = level
            try {
                // Record ownership before publishing so a service restart can invalidate stale data.
                if (preferences.edit().putBoolean("published:${device.address}", true).commit() &&
                    device.setMetadata(BluetoothDevice.METADATA_MAIN_BATTERY,
                        level.toString().toByteArray(Charsets.UTF_8))) {
                    publishedLevels.add(level)
                }
            } catch (e: RuntimeException) {
                Log.w(TAG, "Cannot publish pen battery", e)
            }
            refresh()
        }

        private fun updateNotification(level: Int, charging: Boolean) {
            val key = "alert:${device.address}"
            if (level >= 30) {
                preferences.edit().remove(key).commit()
                return
            }
            val threshold = when {
                level <= 5 -> 2
                level <= 20 -> 1
                else -> 0
            }
            if (charging || threshold <= preferences.getInt(key, 0)) return
            val intent = Intent(Settings.ACTION_BLUETOOTH_SETTINGS)
            val content = PendingIntent.getActivityAsUser(context, 0, intent,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT, null, user)
            val text = if (threshold == 2) R.string.pen_battery_critical else R.string.pen_battery_low
            val notification = Notification.Builder(userContext, CHANNEL)
                .setSmallIcon(R.drawable.ic_stylus)
                .setContentTitle(context.getString(R.string.pen_battery_low_title))
                .setContentText(context.getString(text, level))
                .setContentIntent(content).setAutoCancel(true).build()
            notifications.notify(device.address, NOTIFICATION_ID, notification)
            preferences.edit().putInt(key, threshold).commit()
        }

        private fun cancelNotification() = notifications.cancel(device.address, NOTIFICATION_ID)

        private fun closeGatt() {
            session.batteryInterest(null)
        }

        fun close() {
            closed = true
            handler.removeCallbacks(fallback)
            closeGatt()
            cancelNotification()
            invalidatePublishedLevels(device.address)
        }
    }

    companion object {
        private const val TAG = "PenBattery"
        private const val CHANNEL = "LenovoPenAlerts"
        private const val NOTIFICATION_ID = 1001
        private const val FALLBACK_DELAY_MS = 15000L
    }
}
