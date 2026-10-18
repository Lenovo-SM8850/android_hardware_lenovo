/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import java.util.ArrayDeque
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

/** The only GATT client/ATT queue for an address, shared by BAS and writing. */
internal class PenGattSessions(private val context: Context) {
    private val thread = HandlerThread("PenGattSession").apply { start() }
    private val handler = Handler(thread.looper)
    private val sessions = mutableMapOf<String, PenGattSession>()

    fun get(device: BluetoothDevice): PenGattSession = sessions.getOrPut(device.address) {
        PenGattSession(context, device, handler)
    }

    fun destroy() {
        // Drain best-effort stop/master-off through the queue before closing clients.
        sessions.values.forEach { it.destroy() }
        handler.postDelayed({ thread.quitSafely() }, 3500)
        sessions.clear()
    }
}

internal class PenGattSession(private val context: Context, private val device: BluetoothDevice,
    private val handler: Handler) {
    private data class Operation(val uuid: UUID, val bytes: ByteArray? = null,
        val descriptor: BluetoothGattDescriptor? = null, val effect: Boolean = false,
        val haptic: Boolean = true, val contactGeneration: Long = -1,
        val complete: () -> Unit = {})

    private var gatt: BluetoothGatt? = null
    private var discovered = false
    private var blocked = false
    private var destroyed = false
    private var terminating = false
    private var verified = false
    private var enabled = false
    private var ready = false
    private var epoch = 0L
    private var desired: ByteArray? = null
    private var desiredGeneration = 0L
    private val contactGeneration = AtomicLong()
    @Volatile private var requestedHaptics = false
    private var applied: ByteArray? = null
    private var battery: ((ByteArray) -> Unit)? = null
    private var batteryConfigured = false
    private var timing: PenTpTiming.State? = null
    private var hapticSetupFailed = false
    private var hapticStarted = false
    private val queue = ArrayDeque<Operation>()
    private var inFlight: Operation? = null
    private var retries = 0
    private val deadline = Runnable { fail("operation timeout") }
    private val connectionDeadline = Runnable { fail("connection/discovery timeout") }
    private val terminationDeadline = Runnable { close() }

    fun batteryInterest(listener: ((ByteArray) -> Unit)?, pro2: Boolean = false) = handler.post {
        verified = verified || (pro2 && device.bondState == BluetoothDevice.BOND_BONDED)
        if (battery == null && listener != null) blocked = false
        battery = listener
        if (listener != null) {
            ensureConnected()
            if (discovered) setupBattery()
        } else {
            queue.removeIf { !it.haptic }
            closeIfUnused()
        }
    }

    /** Identity must originate in a bonded HID InputDevice or trusted TP report. */
    fun hapticInterest(pro2: Boolean, active: Boolean, freshLifecycle: Boolean = false): Boolean {
        requestedHaptics = pro2 && active
        if (!requestedHaptics) contactGeneration.incrementAndGet()
        return handler.post {
            // Retain the proof for teardown of this address after input/HID removal.
            verified = verified || pro2
            val next = pro2 && active && device.bondState == BluetoothDevice.BOND_BONDED
            if (next && !enabled) blocked = false
            if (freshLifecycle) { blocked = false; hapticSetupFailed = false }
            if (enabled == next) {
                if (next && freshLifecycle) ensureConnected()
                return@post
            }
            enabled = next
            epoch++
            ready = false
            desired = null
            applied = null
            timing = null
            queue.removeIf { it.haptic }
            if (next) {
                ensureConnected()
                if (discovered) setupHaptics()
            } else {
                stopAndDisarm()
            }
        }
    }

    fun contact(bytes: ByteArray?) {
        val immutable = bytes?.copyOf()
        val generation = contactGeneration.incrementAndGet()
        val at = SystemClock.uptimeMillis()
        handler.post {
            if (destroyed) return@post
            if (immutable != null && generation != contactGeneration.get()) return@post
            desired = if (enabled && verified && requestedHaptics) immutable else null
            desiredGeneration = generation
            queue.removeIf { it.effect }
            Log.d(TAG, "contact t=$at epoch=$epoch strokeGen=$generation desired=${desired?.hex()} depth=${queue.size}")
            if (ready) reconcileEffect()
        }
    }

    fun timingChanged() = handler.post {
        if (!enabled || !discovered || timing == null) return@post
        if (PenTpTiming().read() == timing) return@post
        epoch++
        ready = false
        desired = null
        applied = null
        queue.removeIf { it.haptic }
        setupHaptics()
    }

    private fun ensureConnected() {
        if (destroyed || blocked || terminating || gatt != null || (!enabled && battery == null)) return
        try {
            gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE,
                BluetoothDevice.PHY_LE_1M_MASK, handler)
            if (gatt == null) fail("connect submission")
            else handler.postDelayed(connectionDeadline, 15000)
        } catch (e: RuntimeException) { fail("connect: $e") }
    }

    private fun characteristic(uuid: UUID): BluetoothGattCharacteristic? =
        gatt?.getService(when (uuid) {
            LEVEL -> BAS
            PenHapticProtocol.TIMING -> PenHapticProtocol.TIMING_SERVICE
            else -> PenHapticProtocol.SERVICE
        })?.getCharacteristic(uuid)

    private fun setupHaptics() {
        if (!enabled || !requestedHaptics || !verified || !discovered || hapticSetupFailed || terminating) return
        val client = gatt ?: return
        val service = client.getService(PenHapticProtocol.SERVICE)
        val continuous = service?.getCharacteristic(PenHapticProtocol.CONTINUOUS)
        val master = service?.getCharacteristic(PenHapticProtocol.SWITCH)
        val request = service?.getCharacteristic(PenHapticProtocol.REQUEST)
        val info = service?.getCharacteristic(PenHapticProtocol.INFO)
        val tp = client.getService(PenHapticProtocol.TIMING_SERVICE)
            ?.getCharacteristic(PenHapticProtocol.TIMING)
        val cccd = info?.getDescriptor(PenHapticProtocol.CCCD)
        if (listOf(continuous, master, request, tp).any {
                it == null || PenHapticProtocol.writeType(it) == null } || info == null ||
            info.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0 || cccd == null ||
            !client.setCharacteristicNotification(info, true)) {
            hapticSetupFailed = true
            Log.w(TAG, "unsupported Pro 2 endpoints/properties; BAS remains available")
            return
        }
        val setupEpoch = epoch
        hapticStarted = true
        // Reset any state left by a previous app instance before notification/timing setup.
        queue.add(Operation(PenHapticProtocol.CONTINUOUS, PenHapticProtocol.stop()))
        queue.add(Operation(PenHapticProtocol.SWITCH, byteArrayOf(0)))
        queue.add(Operation(info.uuid, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE.copyOf(), cccd))
        queue.add(Operation(PenHapticProtocol.REQUEST, byteArrayOf(1), complete = {
            awaitTiming(setupEpoch, SystemClock.uptimeMillis() + 3000)
        }))
        pump()
    }

    private fun awaitTiming(setupEpoch: Long, until: Long) {
        if (!enabled || !requestedHaptics || setupEpoch != epoch || !discovered) return
        val state = PenTpTiming().read()
        if (state == null) {
            if (SystemClock.uptimeMillis() >= until) fail("TP host submission unavailable")
            else handler.postDelayed({ awaitTiming(setupEpoch, until) }, 100)
            return
        }
        timing = state
        queue.add(Operation(PenHapticProtocol.TIMING,
            ("0700" + state.timing).toByteArray(Charsets.US_ASCII), complete = {
                handler.postDelayed({
                    if (enabled && requestedHaptics && setupEpoch == epoch) {
                        queue.add(Operation(PenHapticProtocol.SWITCH, byteArrayOf(1), complete = {
                            if (enabled && requestedHaptics && setupEpoch == epoch) {
                                ready = true
                                Log.i(TAG, "ready epoch=$epoch address=…${device.address.takeLast(5)} " +
                                    "VID=17ef PID=622e TP=$state (host submission only)")
                                reconcileEffect()
                            }
                        }))
                        pump()
                    }
                }, 150)
            }))
        pump()
    }

    private fun reconcileEffect() {
        if (!ready || !enabled) return
        if (desired != null && (!requestedHaptics || desiredGeneration != contactGeneration.get())) return
        val next = desired ?: PenHapticProtocol.stop()
        if (applied?.contentEquals(next) == true &&
            inFlight?.uuid != PenHapticProtocol.CONTINUOUS &&
            queue.none { it.uuid == PenHapticProtocol.CONTINUOUS }) return
        queue.removeIf { it.effect }
        // A stop is not coalesced away by a following DOWN.
        val op = Operation(PenHapticProtocol.CONTINUOUS, next.copyOf(), effect = desired != null,
            contactGeneration = desiredGeneration,
            complete = { applied = next.copyOf() })
        if (desired == null) {
            if (queue.none { it.uuid == PenHapticProtocol.CONTINUOUS && !it.effect }) queue.addFirst(op)
        } else queue.addLast(op)
        pump()
    }

    private fun stopAndDisarm() {
        val disarmEpoch = epoch
        if (discovered && verified && hapticStarted && characteristic(PenHapticProtocol.CONTINUOUS) != null &&
            characteristic(PenHapticProtocol.SWITCH) != null) {
            queue.addFirst(Operation(PenHapticProtocol.SWITCH, byteArrayOf(0), complete = {
                if (epoch == disarmEpoch) hapticStarted = false
                closeIfUnused()
            }))
            queue.addFirst(Operation(PenHapticProtocol.CONTINUOUS, PenHapticProtocol.stop()))
            pump()
        } else closeIfUnused()
    }

    private fun setupBattery() {
        if (battery == null || batteryConfigured) return
        val c = gatt?.getService(BAS)?.getCharacteristic(LEVEL)
        if (c == null || c.properties and BluetoothGattCharacteristic.PROPERTY_READ == 0) {
            battery = null
            closeIfUnused()
            return
        }
        batteryConfigured = true
        queue.add(Operation(LEVEL, haptic = false, complete = {
            // Non-Pro-2 fallback is read-only: never write even a BAS CCCD.
            if (!verified) {
                battery = null
                closeIfUnused()
                return@Operation
            }
            if (battery == null) return@Operation
            val descriptor = c.getDescriptor(PenHapticProtocol.CCCD)
            if (c.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0 ||
                descriptor == null || gatt?.setCharacteristicNotification(c, true) != true) {
                battery = null
                closeIfUnused()
                return@Operation
            }
            queue.add(Operation(LEVEL, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE.copyOf(),
                descriptor, haptic = false))
        }))
        pump()
    }

    private fun pump() {
        if (inFlight != null || !discovered || queue.isEmpty()) return
        val op = queue.removeFirst()
        if ((op.effect && (!requestedHaptics || op.contactGeneration != contactGeneration.get())) ||
            (op.uuid == PenHapticProtocol.SWITCH && op.bytes?.singleOrNull() == 1.toByte() &&
                !requestedHaptics)) {
            pump()
            return
        }
        inFlight = op
        retries = 0
        submit()
    }

    private fun submit() {
        val op = inFlight ?: return
        val client = gatt ?: return
        if (op.bytes != null && (!verified || device.bondState != BluetoothDevice.BOND_BONDED)) {
            // Applies to all characteristic/descriptor writes, including BAS.
            fail("write requires a bonded, verified Pro 2")
            return
        }
        val c = characteristic(op.uuid)
        val type = c?.let { PenHapticProtocol.writeType(it) }
        try {
            val result = when {
                op.descriptor != null -> client.writeDescriptor(op.descriptor, op.bytes!!)
                c == null -> -1
                op.bytes == null -> if (client.readCharacteristic(c)) BluetoothStatusCodes.SUCCESS else -1
                type == null -> -1
                else -> client.writeCharacteristic(c, op.bytes, type)
            }
            Log.d(TAG, "submit t=${SystemClock.uptimeMillis()} epoch=$epoch uuid=${op.uuid} " +
                "bytes=${op.bytes?.hex()} type=$type strokeGen=${op.contactGeneration} immediate=$result retry=$retries depth=${queue.size}")
            if (result == BluetoothStatusCodes.SUCCESS) handler.postDelayed(deadline, 3000)
            else retryOrFail("submission rejected $result")
        } catch (e: RuntimeException) { fail("ATT operation: $e") }
    }

    private fun retryOrFail(reason: String) {
        val op = inFlight ?: return
        if (!op.haptic) {
            Log.w(TAG, "BAS unavailable: $reason; releasing battery interest")
            inFlight = null
            battery = null
            batteryConfigured = false
            queue.removeIf { !it.haptic }
            pump()
            closeIfUnused()
            return
        }
        // Never retry a contact whose stroke may already have ended.
        if (terminating) { close(); return }
        if (op.effect || retries >= 3 || (op.haptic && !enabled)) {
            fail(reason)
        } else {
            val delay = 200L shl retries++
            handler.postDelayed({ if (inFlight === op) submit() }, delay)
        }
    }

    private fun complete(status: Int) {
        val op = inFlight ?: return
        handler.removeCallbacks(deadline)
        Log.d(TAG, "callback t=${SystemClock.uptimeMillis()} uuid=${op.uuid} status=$status epoch=$epoch")
        if (status != BluetoothGatt.GATT_SUCCESS) { retryOrFail("callback $status"); return }
        inFlight = null
        op.complete()
        pump()
        closeIfUnused()
    }

    private fun closeIfUnused() {
        if (!enabled && battery == null && inFlight == null && queue.isEmpty()) close()
    }

    private fun fail(reason: String) {
        Log.w(TAG, "close reason=$reason epoch=$epoch address=…${device.address.takeLast(5)}")
        blocked = true
        if (terminating) { close(); return }
        epoch++
        ready = false
        desired = null
        applied = null
        handler.removeCallbacks(deadline)
        handler.removeCallbacks(connectionDeadline)
        queue.clear()
        inFlight = null
        if (verified && discovered && hapticStarted && characteristic(PenHapticProtocol.CONTINUOUS) != null &&
            characteristic(PenHapticProtocol.SWITCH) != null) {
            // No further starts are permitted. A wedged ATT link may reject this best effort.
            terminating = true
            queue.add(Operation(PenHapticProtocol.CONTINUOUS, PenHapticProtocol.stop()))
            queue.add(Operation(PenHapticProtocol.SWITCH, byteArrayOf(0), complete = { close() }))
            handler.postDelayed(terminationDeadline, 3000)
            pump()
        } else {
            close()
        }
    }

    private fun close() {
        epoch++
        contactGeneration.incrementAndGet()
        ready = false
        terminating = false
        discovered = false
        batteryConfigured = false
        hapticStarted = false
        verified = false
        timing = null
        desired = null
        applied = null
        queue.clear()
        inFlight = null
        handler.removeCallbacks(deadline)
        handler.removeCallbacks(connectionDeadline)
        handler.removeCallbacks(terminationDeadline)
        val old = gatt
        gatt = null
        old?.close()
    }

    fun destroy(): Boolean {
        requestedHaptics = false
        contactGeneration.incrementAndGet()
        return handler.post {
            destroyed = true
            enabled = false
            battery = null
            epoch++
            desired = null
            ready = false
            queue.clear()
            stopAndDisarm()
            handler.postDelayed({ close() }, 3200)
        }
    }

    private fun event(client: BluetoothGatt, action: () -> Unit) {
        handler.post {
            if (gatt !== client) return@post
            try { action() } catch (e: RuntimeException) { fail("callback: $e") }
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(client: BluetoothGatt, status: Int, newState: Int) = event(client) {
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                if (!client.discoverServices()) fail("discover submission")
            } else if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                fail("disconnect status=$status")
            }
        }
        override fun onServicesDiscovered(client: BluetoothGatt, status: Int) = event(client) {
            handler.removeCallbacks(connectionDeadline)
            if (status != BluetoothGatt.GATT_SUCCESS) { fail("discovery $status"); return@event }
            discovered = true
            setupHaptics()
            setupBattery()
        }
        override fun onCharacteristicWrite(client: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) = event(client) {
            if (inFlight?.uuid == c.uuid && inFlight?.descriptor == null) complete(status)
        }
        override fun onDescriptorWrite(client: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) = event(client) {
            if (inFlight?.descriptor === d) complete(status)
        }
        override fun onCharacteristicRead(client: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) = event(client) {
            if (inFlight?.uuid != c.uuid || inFlight?.bytes != null) return@event
            if (status == BluetoothGatt.GATT_SUCCESS && c.uuid == LEVEL) battery?.invoke(value.copyOf())
            complete(status)
        }
        override fun onCharacteristicChanged(client: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) = event(client) {
            if (c.uuid == LEVEL) battery?.invoke(value.copyOf())
            if (verified && c.uuid == PenHapticProtocol.INFO) Log.d(TAG, "info ${value.hex()}")
        }
    }

    companion object {
        private const val TAG = "PenGattSession"
        private val BAS = UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb")
        private val LEVEL = UUID.fromString("00002a19-0000-1000-8000-00805f9b34fb")
        private fun ByteArray.hex() = joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
