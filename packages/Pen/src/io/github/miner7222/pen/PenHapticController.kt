/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.app.ActivityTaskManager
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.PowerManager
import android.os.UserHandle
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
    // Service-owned proof survives boot; Settings is never needed to authorize haptics.
    private val identities = context.createDeviceProtectedStorageContext()
        .getSharedPreferences("pen_haptic_identities", Context.MODE_PRIVATE)
    private var selected: String? = null
    private var session: PenGattSession? = null
    private var active = false
    private var pointer = -1
    private var contactDevice = -1
    private var stroke = 0L
    private var hoverArmed = false
    private var hoverEligible = false
    // Stock defers the TP haptics state while the pen is on the panel.
    private var touching = false
    private var pendingTp: Boolean? = null
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
        requestTp(false)
        context.contentResolver.registerContentObserver(
            android.provider.Settings.Secure.getUriFor(PenSettings.WRITING_HAPTICS),
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
        if (!connected && try { adapter?.getRemoteDevice(address)?.bondState == BluetoothDevice.BOND_NONE }
            catch (_: RuntimeException) { false }) {
            identities.edit().remove(address).apply()
        }
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
        val observed = devices.filter { it.isExternal && !it.isVirtual &&
            it.vendorId == PenHapticProtocol.VID && it.productId == PenHapticProtocol.PID }
            .mapNotNull { try { it.bluetoothAddress } catch (_: RuntimeException) { null } }.toSet()
        val bondedDevices = try { adapter?.takeIf { it.isEnabled }?.bondedDevices }
            catch (_: RuntimeException) { null }
        val bonded = bondedDevices?.map { it.address }?.toSet().orEmpty()
        // Never restore an unbonded address, and revoke proof on an observed model change.
        val revoked = devices.filter { it.isExternal && !it.isVirtual &&
            (it.vendorId != PenHapticProtocol.VID || it.productId != PenHapticProtocol.PID) }
            .mapNotNull { try { it.bluetoothAddress } catch (_: RuntimeException) { null } }.toSet()
        val remembered = identities.all.filterValues { it == true }.keys
        val remove = (if (bondedDevices != null) remembered - bonded else emptySet()) + revoked
        val add = (observed intersect bonded) - remembered
        if (remove.isNotEmpty() || add.isNotEmpty()) {
            identities.edit().apply {
                remove.forEach { remove(it) }
                add.forEach { putBoolean(it, true) }
            }.apply()
        }
        // An asleep HID may not have an InputDevice yet. Require a live HID link as
        // well as previously observed VID/PID, never a name or generic stylus metadata.
        val candidates = observed + ((remembered - remove) intersect hid intersect bonded)
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
        val enabled = device != null && displayReady() && PenSettings.writingHaptics(context, user)
        if (selected != device?.address || active != enabled) {
            stop("selection/enablement")
            session?.hapticInterest(true, false)
            active = enabled
            selected = device?.address
            session = device?.let { sessions.get(it) }
            requestTp(enabled)
        }
        session?.hapticInterest(true, enabled, fresh)
    }

    fun motion(event: MotionEvent) {
        if (!supported || destroyed) return
        // Cleanup precedes source/tool filters: CANCEL need not retain stylus metadata.
        when (event.actionMasked) {
            MotionEvent.ACTION_CANCEL -> {
                stop("cancel")
                hoverEligible = false
                endTouch()
                return
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val ours = event.deviceId == contactDevice &&
                    event.getPointerId(event.actionIndex) == pointer
                if (ours) stop("up")
                if (ours || event.actionMasked == MotionEvent.ACTION_UP) endTouch()
                return
            }
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                hover(event)
                return
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                hoverExit(event)
                return
            }
        }
        if (event.actionMasked != MotionEvent.ACTION_DOWN &&
            event.actionMasked != MotionEvent.ACTION_POINTER_DOWN) return
        if (!displayReady() || event.displayId != Display.DEFAULT_DISPLAY ||
            event.device?.name != inputName || !event.isFromSource(InputDevice.SOURCE_STYLUS)) return
        val tool = event.getToolType(event.actionIndex)
        if (tool != MotionEvent.TOOL_TYPE_STYLUS && tool != MotionEvent.TOOL_TYPE_ERASER) return
        // Stock marks the panel as touched before checking whether haptics apply.
        touching = true
        // A fresh eligible contact may recover a timed-out client, never a MOVE/finger.
        refresh(true)
        if (!active || !hapticAppMatch()) {
            disarmHover()
            return
        }
        if (!hoverArmed) stop("new contact")
        hoverArmed = false
        pointer = event.getPointerId(event.actionIndex)
        contactDevice = event.deviceId
        stroke++
        if (DEBUG) Log.d(TAG, "down user=$user display=${event.displayId} stroke=$stroke tool=$tool VID=17ef PID=622e")
        session?.contact(PenHapticProtocol.contact(effectiveTool(event), hapticLevel()))
    }

    /** Stock arms continuous haptics on hover so the first contact already vibrates. */
    private fun hover(event: MotionEvent) {
        if (!displayReady() || event.displayId != Display.DEFAULT_DISPLAY ||
            event.device?.name != inputName || !event.isFromSource(InputDevice.SOURCE_STYLUS)) return
        val tool = event.getToolType(event.actionIndex)
        if (tool != MotionEvent.TOOL_TYPE_STYLUS && tool != MotionEvent.TOOL_TYPE_ERASER) return
        if (event.actionMasked == MotionEvent.ACTION_HOVER_ENTER) {
            refresh(true)
            hoverEligible = active && hapticAppMatch()
        }
        val distance = event.getAxisValue(MotionEvent.AXIS_DISTANCE, event.actionIndex)
        if (distance > HOVER_DISARM_DISTANCE) {
            disarmHover()
            return
        }
        if (!hoverEligible || !active) return
        if (distance <= HOVER_ARM_DISTANCE && !hoverArmed) {
            hoverArmed = true
            session?.contact(PenHapticProtocol.contact(effectiveTool(event), hapticLevel()))
            if (DEBUG) Log.d(TAG, "hover arm distance=$distance tool=$tool user=$user")
        }
    }

    private fun hoverExit(event: MotionEvent) {
        if (hoverArmed && event.displayId == Display.DEFAULT_DISPLAY &&
            event.device?.name == inputName && event.isFromSource(InputDevice.SOURCE_STYLUS)) {
            disarmHover()
        }
        hoverEligible = false
    }

    private fun disarmHover() {
        if (!hoverArmed) return
        hoverArmed = false
        session?.contact(null)
    }

    private fun endTouch() {
        if (!touching && pendingTp == null) return
        touching = false
        val pending = pendingTp ?: return
        pendingTp = null
        tp.request(pending)
        if (DEBUG) Log.d(TAG, "deferred TP haptics=$pending written on pen up")
    }

    private fun requestTp(enabled: Boolean) {
        if (touching) {
            pendingTp = enabled
            return
        }
        pendingTp = null
        tp.request(enabled)
    }

    private fun effectiveTool(event: MotionEvent): Int =
        event.getToolType(event.actionIndex)

    private fun hapticLevel() = PenSettings.hapticLevel(context, user)

    private fun hapticAppMatch(): Boolean {
        // RRO changes update Resources without restarting this persistent service.
        val hapticPackages = context.resources.getStringArray(R.array.config_penHapticPackages)
        if (hapticPackages.isEmpty()) return false
        val pkg = try {
            ActivityTaskManager.getInstance().getTasks(1).firstOrNull()?.topActivity?.packageName
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot read the foreground app for pen haptics", e)
            null
        }
        return pkg != null && pkg in hapticPackages
    }

    private fun stop(reason: String) {
        stroke++
        pointer = -1
        contactDevice = -1
        hoverArmed = false
        session?.contact(null)
        if (DEBUG) Log.d(TAG, "stop reason=$reason stroke=$stroke user=$user")
    }

    fun suspend(reason: String) {
        if (!supported) return
        stop(reason)
        hoverEligible = false
        active = false
        session?.hapticInterest(true, false)
        requestTp(false)
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
        endTouch()
        destroyed = true
        context.contentResolver.unregisterContentObserver(observer)
        displays.unregisterDisplayListener(displayListener)
    }

    companion object {
        private const val DEBUG = false
        private const val TAG = "PenHaptic"
        // Stock hover pre-arm thresholds, in the panel's AXIS_DISTANCE units.
        private const val HOVER_ARM_DISTANCE = 30f
        private const val HOVER_DISARM_DISTANCE = 65f
    }
}
