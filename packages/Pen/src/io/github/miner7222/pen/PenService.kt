/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.input.InputManager
import android.os.Handler
import android.os.IBinder
import android.os.ParcelUuid
import android.os.PowerManager
import android.os.SystemProperties
import android.os.UEventObserver
import android.util.Log
import android.view.Display
import android.view.InputDevice
import android.view.InputEvent
import android.view.InputEventReceiver
import android.view.InputMonitor
import android.view.MotionEvent
import java.util.Locale

class PenService : Service() {
    private val handler by lazy { Handler(mainLooper) }
    private val inputManager by lazy { getSystemService(InputManager::class.java) }
    private val bluetoothManager by lazy { getSystemService(BluetoothManager::class.java) }
    private val notifications by lazy { getSystemService(NotificationManager::class.java) }
    private val powerManager by lazy { getSystemService(PowerManager::class.java) }
    private val cap by lazy { PenRefreshRateCap(this, handler) }
    private val penTypes by lazy {
        val format = Regex("([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})=([0-9]+)")
        resources.getStringArray(R.array.config_penIdentityToControllerType).mapNotNull { entry ->
            val match = format.matchEntire(entry)
            val value = match?.groupValues?.get(3)?.toIntOrNull()?.takeIf { it in 0..255 }
            if (match == null || value == null) {
                Log.w(TAG, "Invalid pen type mapping: $entry")
                null
            } else {
                (match.groupValues[1].toInt(16) to match.groupValues[2].toInt(16)) to value
            }
        }.toMap()
    }
    private val inputName by lazy { getString(R.string.config_penInputName) }
    private val penDevPath by lazy { getString(R.string.config_penUEventDevPath) }
    private val penUEventTarget by lazy { getString(R.string.config_penUEventTarget) }
    private val earlyChangeActivity by lazy { resources.getBoolean(R.bool.config_penEarlyChangeActivity) }
    private var monitor: InputMonitor? = null
    private var inputReceiver: InputEventReceiver? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanCallback: ScanCallback? = null
    private var discoveryScanner: BluetoothLeScanner? = null
    private var discoveryCallback: ScanCallback? = null
    private var pen: PenInfo? = null
    private var attemptedAddress: String? = null
    private var pairingAddress: String? = null
    private var pairing = false
    private var status: Int? = null
    private var destroyed = false
    private var sessionActive = false
    private var connectedPen = false
    private val release = Runnable {
        if (connectedPen && powerManager.isInteractive) {
            // Stock expires only the display request; keep the connected pen ready.
            cap.releaseRefresh()
            return@Runnable
        }
        sessionActive = false
        cap.release()
        notifications.cancel(NOTIFICATION_ID)
        if (!pairing) attemptedAddress = null
    }
    private val pairingTimeout = Runnable { finishPairing(R.string.pen_pair_failed) }
    private val retryMonitor = Runnable { startInputMonitor() }

    private val observer = object : UEventObserver() {
        override fun onUEvent(event: UEvent) {
            if (penDevPath.isEmpty() || penUEventTarget.isEmpty() ||
                event.get("UEVENT_TO") != penUEventTarget ||
                event.get("DEVPATH") != penDevPath || event.get("ACTION") != "change") return
            val info = PenInfo.parse(event.get("INFO"), event.get("MAC")) ?: return
            handler.post {
                if (destroyed) return@post
                if (info.identified) selectPenType(info)
                if (!powerManager.isInteractive) return@post
                if (!info.identified) {
                    // Only a verified controller report path may cap before identity.
                    if (earlyChangeActivity) markActivity(false)
                    return@post
                }
                val samePairingAddress = pairing && info.address != null && pairingAddress == info.address
                if (!samePairingAddress &&
                    (pen?.identity != info.identity || (info.address != null && pen?.address != info.address))) {
                    stopPairing()
                    attemptedAddress = null
                    status = null
                }
                pen = info
                // Identity/ready reports can arrive before the first routed MotionEvent.
                markActivity()
                showNotification()
                if (info.bleSupported && info.address != null && attemptedAddress != info.address) {
                    pair(info.address)
                }
            }
        }
    }

    private val deviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = checkPenDevice()
        override fun onInputDeviceChanged(deviceId: Int) = checkPenDevice()
        override fun onInputDeviceRemoved(deviceId: Int) = checkPenDevice()
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                Intent.ACTION_SCREEN_ON -> checkPenDevice()
                Intent.ACTION_SCREEN_OFF -> endSession()
                Intent.ACTION_USER_SWITCHED -> {
                    endSession()
                    checkPenDevice()
                }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                    val device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    if (device?.address != pairingAddress) return
                    when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                        BluetoothDevice.BOND_BONDED -> finishPairing(R.string.pen_paired)
                        BluetoothDevice.BOND_NONE -> finishPairing(R.string.pen_pair_failed)
                    }
                }
                android.bluetooth.BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    if (bluetoothManager.adapter?.isEnabled != true) {
                        stopPairing()
                        stopDiscovery()
                    } else {
                        startDiscovery()
                        pen?.takeIf { powerManager.isInteractive && it.bleSupported }?.address?.let {
                            attemptedAddress = null
                            pair(it)
                        }
                    }
                }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        cap.recover()
        notifications.createNotificationChannel(NotificationChannel(CHANNEL,
            getString(R.string.pen_notification_channel), NotificationManager.IMPORTANCE_LOW))
        registerReceiver(receiver, IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_USER_SWITCHED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            addAction(android.bluetooth.BluetoothAdapter.ACTION_STATE_CHANGED)
        }, Context.RECEIVER_EXPORTED)
        inputManager.registerInputDeviceListener(deviceListener, handler)
        if (penDevPath.isNotEmpty() && penUEventTarget.isNotEmpty()) {
            observer.startObserving("DEVPATH=$penDevPath")
        }
        startInputMonitor()
        checkPenDevice()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_PAIR) {
            pen?.takeIf { it.bleSupported }?.address?.let { pair(it) }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun selectPenType(info: PenInfo) {
        // Overlay controller types differ from event report IDs.
        selectPenType(info.vid, info.pid)
    }

    private fun selectPenType(vid: Int, pid: Int) {
        val value = penTypes[vid to pid]?.toString() ?: return
        try {
            if (SystemProperties.get(PEN_TYPE_PROPERTY) != value) {
                SystemProperties.set(PEN_TYPE_PROPERTY, value)
            }
        } catch (e: RuntimeException) {
            // Retry on the next identity report without interrupting input, BLE or the cap.
            Log.w(TAG, "Cannot select controller pen type $value", e)
        }
    }

    private fun startInputMonitor() {
        if (destroyed || monitor != null) return
        try {
            val newMonitor = inputManager.monitorGestureInput("LenovoPen", Display.DEFAULT_DISPLAY)
            monitor = newMonitor
            inputReceiver = object : InputEventReceiver(newMonitor.inputChannel, mainLooper) {
                override fun onInputEvent(event: InputEvent) {
                    try {
                        if (event is MotionEvent && inputName.isNotEmpty() && event.device?.name == inputName &&
                            event.isFromSource(InputDevice.SOURCE_STYLUS) && powerManager.isInteractive &&
                            event.actionMasked != MotionEvent.ACTION_CANCEL) {
                            markActivity()
                        }
                    } finally {
                        // Observe only: never pilfer pointers or consume the app's pen events.
                        finishInputEvent(event, false)
                    }
                }
                override fun onBatchedInputEventPending(source: Int) {
                    consumeBatchedInputEvents(-1)
                }
            }
        } catch (e: RuntimeException) {
            inputReceiver?.dispose()
            inputReceiver = null
            monitor?.dispose()
            monitor = null
            Log.e(TAG, "Cannot monitor pen input; retrying", e)
            handler.postDelayed(retryMonitor, 5000)
        }
    }

    private fun markActivity(showStatus: Boolean = true) {
        if (!sessionActive) {
            sessionActive = true
            if (showStatus) showNotification()
        }
        cap.activate()
        handler.removeCallbacks(release)
        handler.postDelayed(release, PEN_IDLE_MS)
    }

    private fun checkPenDevice() {
        val devices = inputManager.inputDeviceIds.map { inputManager.getInputDevice(it) }.filterNotNull()
        // AP501U's BLE HID exposes mouse/consumer inputs, not SOURCE_STYLUS.
        // Stock enables TP pen mode on HID connection, before any NVT report.
        val device = devices.firstOrNull {
            it.isExternal && !it.isVirtual && penTypes.containsKey(it.vendorId to it.productId)
        }
        val wasConnected = connectedPen
        connectedPen = device != null
        if (device != null) {
            stopDiscovery()
            if (!wasConnected) Log.i(TAG, "Connected pen HID: ${device.name}")
            selectPenType(device.vendorId, device.productId)
            if (powerManager.isInteractive) {
                cap.activatePolling()
                // Bootstrap before the first stroke, including screen/user resume.
                // Device changes while connected must not renew pen activity.
                if (!wasConnected || !sessionActive) markActivity(false)
            }
        } else if (wasConnected || devices.none { inputName.isNotEmpty() && it.name == inputName }) {
            if (wasConnected) Log.i(TAG, "Disconnected pen HID")
            endSession()
        }
        if (!connectedPen) startDiscovery()
    }

    private fun startDiscovery() {
        if (destroyed || !powerManager.isInteractive || connectedPen || pairing ||
            discoveryCallback != null || !penTypes.containsKey(0x17ef to 0x617f)) return
        try {
            val adapter = bluetoothManager.adapter ?: return
            if (!adapter.isEnabled) return
            val newScanner = adapter.bluetoothLeScanner ?: return
            val callback = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    val record = result.scanRecord ?: return
                    val data = record.getManufacturerSpecificData(0x02c5) ?: return
                    val info = PenInfo.fromAdvertisement(data) ?: return
                    handler.post {
                        if (discoveryCallback !== this || pen?.address == info.address) return@post
                        pen = info
                        status = null
                        selectPenType(info)
                        Log.i(TAG, "Discovered Tab Pen Plus pairing advertisement")
                        // Use the existing Pair action, as stock's discovery popup does.
                        showNotification()
                    }
                }
                override fun onScanFailed(errorCode: Int) {
                    handler.post {
                        if (discoveryCallback === this) {
                            Log.w(TAG, "Pen discovery failed: $errorCode")
                            stopDiscovery()
                        }
                    }
                }
            }
            discoveryScanner = newScanner
            discoveryCallback = callback
            newScanner.startScan(listOf(ScanFilter.Builder().setServiceUuid(PAIRING_UUID).build()),
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_POWER)
                    .setLegacy(false).build(), callback)
        } catch (e: RuntimeException) {
            stopDiscovery()
            Log.w(TAG, "Cannot discover pen", e)
        }
    }

    private fun stopDiscovery() {
        val callback = discoveryCallback
        discoveryCallback = null
        try {
            if (callback != null) discoveryScanner?.stopScan(callback)
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot stop pen discovery", e)
        }
        discoveryScanner = null
    }

    private fun pair(address: String) {
        if (pairing) return
        stopDiscovery()
        try {
            val adapter = bluetoothManager.adapter ?: return
            if (!adapter.isEnabled) {
                status = R.string.pen_bluetooth_off
                showNotification()
                return
            }
            val device = adapter.getRemoteDevice(address)
            if (device.bondState == BluetoothDevice.BOND_BONDED) {
                attemptedAddress = address
                status = R.string.pen_paired
                showNotification()
                return
            }
            attemptedAddress = address
            pairingAddress = address
            pairing = true
            status = R.string.pen_pairing
            handler.postDelayed(pairingTimeout, PAIR_TIMEOUT_MS)
            if (device.bondState == BluetoothDevice.BOND_BONDING) {
                showNotification()
                return
            }
            if (pen?.identity == "ble:$address") {
                // ZuiUDevice bonds the protocol's public MAC directly. The scan
                // result itself may use a different advertising address.
                if (!device.createBond(BluetoothDevice.TRANSPORT_LE)) {
                    finishPairing(R.string.pen_pair_failed)
                } else {
                    showNotification()
                }
                return
            }
            val newScanner = adapter.bluetoothLeScanner ?: run {
                finishPairing(R.string.pen_pair_failed)
                return
            }
            val callback = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    handler.post {
                        if (scanCallback !== this || result.device.address != address) return@post
                        stopScan()
                        try {
                            // The controller reports an already reversed, colon-separated address.
                            if (!result.device.createBond(BluetoothDevice.TRANSPORT_LE)) {
                                finishPairing(R.string.pen_pair_failed)
                            }
                        } catch (e: RuntimeException) {
                            Log.w(TAG, "Cannot bond pen", e)
                            finishPairing(R.string.pen_pair_failed)
                        }
                    }
                }
                override fun onScanFailed(errorCode: Int) {
                    handler.post {
                        if (scanCallback === this) finishPairing(R.string.pen_pair_failed)
                    }
                }
            }
            scanner = newScanner
            scanCallback = callback
            newScanner.startScan(listOf(ScanFilter.Builder().setDeviceAddress(address).build()),
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .setLegacy(false).build(), callback)
            showNotification()
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot scan for pen", e)
            finishPairing(R.string.pen_pair_failed)
        }
    }

    private fun stopScan() {
        val callback = scanCallback
        scanCallback = null
        try {
            if (callback != null) scanner?.stopScan(callback)
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot stop pen scan", e)
        }
        scanner = null
    }

    private fun stopPairing() {
        stopScan()
        handler.removeCallbacks(pairingTimeout)
        pairing = false
        pairingAddress = null
    }

    private fun finishPairing(message: Int) {
        stopPairing()
        status = message
        if (powerManager.isInteractive) showNotification()
        startDiscovery()
    }

    private fun showNotification() {
        val info = pen ?: return
        try {
            val battery = info.battery?.let { getString(R.string.pen_battery_level, it) }
                ?: getString(R.string.pen_battery_unknown)
            val notification = Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_stylus).setContentTitle(getString(R.string.pen_detected))
                .setContentText(listOfNotNull(battery, status?.let { getString(it) }).joinToString(" · "))
                .setOnlyAlertOnce(true)
            if (info.bleSupported && info.address != null && !pairing && status != R.string.pen_paired) {
                val action = PendingIntent.getService(this, 1,
                    Intent(this, PenService::class.java).setAction(ACTION_PAIR), PendingIntent.FLAG_IMMUTABLE)
                notification.addAction(Notification.Action.Builder(null,
                    getString(R.string.pen_pair_action), action).build())
            }
            notifications.notify(NOTIFICATION_ID, notification.build())
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot post pen notification", e)
        }
    }

    private fun endSession() {
        sessionActive = false
        handler.removeCallbacks(release)
        cap.release()
        stopPairing()
        stopDiscovery()
        notifications.cancel(NOTIFICATION_ID)
        pen = null
        attemptedAddress = null
        status = null
    }

    override fun onDestroy() {
        destroyed = true
        observer.stopObserving()
        inputManager.unregisterInputDeviceListener(deviceListener)
        unregisterReceiver(receiver)
        inputReceiver?.dispose()
        monitor?.dispose()
        endSession()
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private data class PenInfo(val identity: String, val vid: Int, val pid: Int,
        val battery: Int?, val address: String?,
        val bleSupported: Boolean, val identified: Boolean) {
        companion object {
            fun fromAdvertisement(data: ByteArray): PenInfo? {
                // PRC 088 ZuiUDevice: ZuiPairV2, Lenovo company 0x02c5,
                // little-endian check code 0x908570 and Picasso device ID 1.
                if (data.size < 13 || data[0] != 0x70.toByte() || data[1] != 0x85.toByte() ||
                    data[2] != 0x90.toByte() || data[4] != 1.toByte()) return null
                val address = (11 downTo 6).joinToString(":") {
                    "%02X".format(Locale.ROOT, data[it].toInt() and 0xff)
                }
                if (address == "00:00:00:00:00:00" || address == "FF:FF:FF:FF:FF:FF") return null
                return PenInfo("ble:$address", 0x17ef, 0x617f, null, address, true, true)
            }

            fun parse(info: String?, mac: String?): PenInfo? {
                val fields = info?.split(';') ?: return null
                if (fields.size != 5) return null
                val type = fields[0].toIntOrNull()?.takeIf { it in 0..255 && it != 170 } ?: return null
                val battery = fields[1].toIntOrNull()?.takeIf { it in 0..255 } ?: return null
                val pid = fields[2].removePrefix("0x").toIntOrNull(16)?.takeIf { it in 0..65535 } ?: return null
                val vid = fields[3].removePrefix("0x").toIntOrNull(16)?.takeIf { it in 0..65535 } ?: return null
                val sn = fields[4].removePrefix("0x").toIntOrNull(16)?.takeIf { it in 0..0xffffff } ?: return null
                val identified = !(type == 255 && pid == 0xffff && vid == 0xffff && sn == 0x3fffff)
                val address = mac?.uppercase(Locale.ROOT)?.takeIf {
                    it.matches(Regex("(?:[0-9A-F]{2}:){5}[0-9A-F]{2}")) &&
                        it != "00:00:00:00:00:00" && it != "FF:FF:FF:FF:FF:FF"
                }
                // Exclude non-BLE Monet types (4/7/255).
                val supported = vid == 0x17ef && when (type) {
                    1 -> pid == 0x612b
                    2, 3 -> pid == 0x617f
                    5 -> pid == 0x61a1
                    6 -> pid == 0x622e
                    else -> false
                }
                return PenInfo("$type:$pid:$vid:$sn", vid, pid, battery.takeIf { it in 0..100 && type != 255 },
                    address, supported, identified)
            }
        }
    }

    companion object {
        private const val TAG = "LenovoPenService"
        private val PAIRING_UUID = ParcelUuid.fromString("0000f9ea-0000-1000-8000-00805f9b34fb")
        private const val PEN_TYPE_PROPERTY = "sys.lenovo.pen.type"
        private const val PEN_IDLE_MS = 10000L
        private const val PAIR_TIMEOUT_MS = 30000L
        private const val CHANNEL = "LenovoPen"
        private const val NOTIFICATION_ID = 1000
        private const val ACTION_PAIR = "io.github.miner7222.pen.PAIR"
    }
}
