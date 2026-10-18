/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.PowerManager
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import android.view.Display
import android.view.InputDevice
import android.view.MotionEvent

/** Contact policy runs on the service handler; no MotionEvent escapes the input callback. */
internal class PenHapticController(private val context: Context, private val handler: Handler,
    private val sessions: PenGattSessions) {
    private val supported = context.resources.getBoolean(R.bool.config_penWritingHapticsSupported)
    private val adapter = context.getSystemService(BluetoothManager::class.java).adapter
    private val power = context.getSystemService(PowerManager::class.java)
    private val displays = context.getSystemService(DisplayManager::class.java)
    private val inputName = context.getString(R.string.config_penInputName)
    private val tp = PenTpTiming()
    private var selected: String? = null
    private var session: PenGattSession? = null
    private var active = false
    private var pointer = -1
    private var contactDevice = -1
    private var stroke = 0L
    private var user = ActivityManager.getCurrentUser()
    private var devices = emptyList<InputDevice>()
    private var hid = emptySet<String>()
    private var trustedAddress: String? = null
    private var trustedPro2 = false
    private var touchIdentitySeen = false
    private var trustedFullIdentity = false
    private val disconnected = mutableSetOf<String>()
    private var destroyed = false
    private val observer = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) { refresh(false) }
    }
    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(id: Int) { if (id == Display.DEFAULT_DISPLAY) refresh(true) }
        override fun onDisplayRemoved(id: Int) { if (id == Display.DEFAULT_DISPLAY) suspend("display removed") }
        override fun onDisplayChanged(id: Int) { if (id == Display.DEFAULT_DISPLAY) refresh(true) }
    }

    fun start() {
        if (!supported || destroyed) return
        tp.request(false)
        context.contentResolver.registerContentObserver(Settings.Secure.getUriFor(PenHapticProtocol.SETTING),
            false, observer, UserHandle.USER_ALL)
        displays.registerDisplayListener(displayListener, handler)
    }

    fun updateDevices(input: List<InputDevice>, connectedHid: Set<String>) {
        if (devices.any { old -> input.none { it.id == old.id } }) stop("input removed")
        devices = input
        hid = connectedHid.toSet()
        disconnected.removeAll(hid)
        if (pointer != -1 && input.none { it.id == contactDevice }) stop("input removed")
        refresh(true)
    }

    fun touchIdentity(vid: Int, pid: Int, address: String?, controllerType: Int?) {
        touchIdentitySeen = true
        trustedAddress = address
        // Goodix currently supplies type/MAC with unknown VID/PID. Type 6 may
        // corroborate an actual 17ef:622e HID; it cannot create that identity.
        trustedFullIdentity = vid == PenHapticProtocol.VID && pid == PenHapticProtocol.PID
        trustedPro2 = trustedFullIdentity ||
            (controllerType == 6 && vid == 0xffff && pid == 0xffff)
        refresh(true)
    }

    fun timingChanged() { if (active) session?.timingChanged() }

    fun connectionChanged(address: String, connected: Boolean) {
        if (connected) disconnected.remove(address) else disconnected.add(address)
        if (!connected && selected == address) suspend("HID/bond disconnected")
        refresh(true)
    }

    private fun displayReady() = power.isInteractive &&
        displays.getDisplay(Display.DEFAULT_DISPLAY)?.state == Display.STATE_ON

    private fun refresh(fresh: Boolean) {
        if (!supported || destroyed) return
        if (ActivityManager.getCurrentUser() != user) {
            suspend("user changed")
            user = ActivityManager.getCurrentUser()
        }
        val candidates = devices.filter { it.isExternal && !it.isVirtual &&
            it.vendorId == PenHapticProtocol.VID && it.productId == PenHapticProtocol.PID }
            .mapNotNull { try { it.bluetoothAddress } catch (_: RuntimeException) { null } }.toSet()
        val anotherModel = devices.any { it.isExternal && !it.isVirtual &&
            ((it.vendorId == PenHapticProtocol.VID && it.productId == 0x617f) ||
                (it.supportsSource(InputDevice.SOURCE_STYLUS) &&
                    (it.vendorId != PenHapticProtocol.VID || it.productId != PenHapticProtocol.PID))) }
        // The shared panel cannot attribute a stroke when different pen models coexist.
        val address = if (anotherModel || (touchIdentitySeen && !trustedPro2)) null else if (trustedAddress != null) {
            trustedAddress?.takeIf { trustedPro2 && (it in candidates || (trustedFullIdentity && it in hid)) }
        } else candidates.singleOrNull()
        val device = try { address?.takeUnless { it in disconnected }?.let { adapter?.getRemoteDevice(it) }
            ?.takeIf { adapter?.isEnabled == true && it.bondState == BluetoothDevice.BOND_BONDED }
        } catch (_: RuntimeException) { null }
        val enabled = device != null && displayReady() && Settings.Secure.getIntForUser(
            context.contentResolver, PenHapticProtocol.SETTING, 0, user) != 0
        if (selected != device?.address || active != enabled) {
            stop("selection/enablement")
            session?.hapticInterest(true, false)
            active = enabled
            selected = device?.address
            session = device?.let { sessions.get(it) }
            tp.request(enabled)
        }
        session?.hapticInterest(true, enabled, fresh)
    }

    fun motion(event: MotionEvent) {
        if (!supported || destroyed) return
        // Cleanup precedes source/tool filters: CANCEL need not retain stylus metadata.
        when (event.actionMasked) {
            MotionEvent.ACTION_CANCEL -> { stop("cancel"); return }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.deviceId == contactDevice &&
                    event.getPointerId(event.actionIndex) == pointer) stop("up")
                return
            }
        }
        if (event.actionMasked != MotionEvent.ACTION_DOWN &&
            event.actionMasked != MotionEvent.ACTION_POINTER_DOWN) return
        if (!displayReady() || event.displayId != Display.DEFAULT_DISPLAY ||
            event.device?.name != inputName || !event.isFromSource(InputDevice.SOURCE_STYLUS)) return
        val tool = event.getToolType(event.actionIndex)
        if (tool != MotionEvent.TOOL_TYPE_STYLUS && tool != MotionEvent.TOOL_TYPE_ERASER) return
        // A fresh eligible contact may recover a timed-out client, never a MOVE/finger.
        refresh(true)
        if (!active) return
        stop("new contact")
        pointer = event.getPointerId(event.actionIndex)
        contactDevice = event.deviceId
        stroke++
        if (DEBUG) Log.d(TAG, "down user=$user display=${event.displayId} stroke=$stroke tool=$tool VID=17ef PID=622e")
        session?.contact(PenHapticProtocol.contact(tool))
    }

    private fun stop(reason: String) {
        stroke++
        pointer = -1
        contactDevice = -1
        session?.contact(null)
        if (DEBUG) Log.d(TAG, "stop reason=$reason stroke=$stroke user=$user")
    }

    fun suspend(reason: String) {
        if (!supported) return
        stop(reason)
        active = false
        session?.hapticInterest(true, false)
        tp.request(false)
    }

    fun userSwitched() {
        suspend("user switch")
        trustedAddress = null
        trustedPro2 = false
        touchIdentitySeen = false
        trustedFullIdentity = false
        user = ActivityManager.getCurrentUser()
    }

    fun destroy() {
        if (!supported) return
        suspend("service destroyed")
        destroyed = true
        context.contentResolver.unregisterContentObserver(observer)
        displays.unregisterDisplayListener(displayListener)
    }

    companion object { private const val DEBUG = false
        private const val TAG = "PenHaptic" }
}
