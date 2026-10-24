// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.Manifest
import android.content.pm.PackageManager
import android.app.ActivityManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.database.ContentObserver
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.os.ServiceManager
import android.os.SystemClock
import android.os.UserHandle
import android.provider.Settings
import android.service.quicksettings.TileService
import android.util.Log
import vendor.lenovo.hardware.lightring.Effect
import vendor.lenovo.hardware.lightring.ILightRing

object RingController {
    const val DEFAULT_COLOURS = "FF0000,FF8000,FFFF00,00FF00,00FFFF,0000FF,8000FF,FF00FF,FFFFFF"
    private const val SERVICE = "vendor.lenovo.hardware.lightring.ILightRing/default"
    private lateinit var context: Context
    lateinit var preferences: SharedPreferences
        private set
    private lateinit var handler: Handler
    private lateinit var session: ReactiveSession
    private lateinit var pulses: AudioPulseWorker
    private var remote: ILightRing? = null
    private var binder: IBinder? = null
    private var lastEffect: String? = null
    private var scene = "idle"
    private var outputMode: String? = null
    private var pulseLevel: Int? = null
    private var clearPending = false
    private var retryAt = 0L
    @Volatile var connected = false
        private set
    private val applyState = Runnable { apply() }
    private val preferenceListener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
        refresh()
        updateTile()
    }
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            refresh()
            updateTile()
        }
    }

    fun initialize(app: Context) {
        if (::context.isInitialized) return
        context = app.createDeviceProtectedStorageContext()
        preferences = context.getSharedPreferences("light_ring", Context.MODE_PRIVATE)
        handler = Handler(HandlerThread("HaloRingWorker").apply { start() }.looper)
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return
        Settings.Secure.putIntForUser(context.contentResolver, ReactiveSettings.AVAILABLE,
            0, UserHandle.USER_SYSTEM)
        session = ReactiveSession(context, handler) { refresh() }
        pulses = AudioPulseWorker(handler, { level ->
            pulseLevel = level
            apply()
        }, {
            pulseLevel = null
            refresh()
        })
        preferences.registerOnSharedPreferenceChangeListener(preferenceListener)
        context.contentResolver.registerContentObserver(
            Settings.Secure.getUriFor(ReactiveSettings.ENABLED), false,
            object : ContentObserver(handler) {
                override fun onChange(selfChange: Boolean) = refresh()
            }, UserHandle.USER_SYSTEM
        )
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_USER_SWITCHED)
            addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
        }, Context.RECEIVER_NOT_EXPORTED)
        refresh()
    }

    fun parseColours(value: String): IntArray? {
        val parts = value.split(',').map { it.trim() }
        if (parts.size !in 1..9 || parts.any { !it.matches(Regex("[0-9a-fA-F]{6}")) }) return null
        return parts.map { it.toInt(16) }.toIntArray()
    }

    fun enabled() = preferences.getBoolean("enabled", false)
    fun allowed(): Boolean {
        val power = context.getSystemService(PowerManager::class.java)
        return UserHandle.myUserId() == UserHandle.USER_SYSTEM &&
            ActivityManager.getCurrentUser() == UserHandle.USER_SYSTEM &&
            power.isInteractive && !power.isPowerSaveMode
    }
    fun refresh() {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return
        handler.removeCallbacks(applyState)
        handler.post(applyState)
    }
    private fun updateTile() {
        TileService.requestListeningState(context, ComponentName(context, LegionHaloTile::class.java))
    }
    private fun connectionChanged(value: Boolean) {
        if (connected != value) {
            connected = value
            Settings.Secure.putIntForUser(context.contentResolver, ReactiveSettings.AVAILABLE,
                if (value) 1 else 0, UserHandle.USER_SYSTEM)
            updateTile()
        }
    }

    private fun apply() {
        val allowed = allowed()
        val music = preferences.getBoolean("react_music", true)
        val games = preferences.getBoolean("react_games", true)
        session.track(allowed && ReactiveSettings.enabled(context) && (music || games))
        val nextScene = session.scene(music, games)
        if (scene != nextScene) {
            Log.i("HaloReactive", "Scene $scene -> $nextScene")
            scene = nextScene
            pulses.stop(resetFailure = true)
            pulseLevel = null
        }
        val reactive = scene != "idle" && !session.failed
        val audio = reactive && session.hasAudio(music, games) &&
            preferences.getBoolean("react_rhythm", true) &&
            context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        if (audio && SystemClock.uptimeMillis() >= retryAt) {
            pulses.start()
        } else {
            pulses.stop(resetFailure = !reactive || !audio)
            pulseLevel = null
        }
        if (!allowed || !ReactiveSettings.enabled(context)) {
            pulses.stop(resetFailure = true)
            pulseLevel = null
        }

        val palette = parseColours(preferences.getString("colours", DEFAULT_COLOURS)!!)
            ?: parseColours(DEFAULT_COLOURS)!!
        val brightness = preferences.getInt("brightness", 128).coerceIn(0, 255)
        val effect = when {
            !allowed -> null
            reactive && pulseLevel != null -> Effect().apply {
                type = 0
                colors = intArrayOf(palette[0])
                speed = 1
                this.brightness = brightness * pulseLevel!! / 15
            }
            enabled() -> Effect().apply {
                type = preferences.getString("effect", "2")!!.toInt().coerceIn(0, 3)
                colors = if (type < 2) intArrayOf(palette[0]) else palette
                speed = preferences.getString("speed", "1")!!.toInt().coerceIn(0, 2)
                this.brightness = brightness
            }
            reactive -> Effect().apply {
                type = 2
                colors = palette
                speed = 1
                this.brightness = brightness
            }
            else -> null
        }
        val mode = when {
            effect == null -> "off"
            reactive && pulseLevel != null -> "$scene pulse"
            reactive -> "$scene fallback ${effect.type}"
            else -> "manual ${effect.type}"
        }
        if (outputMode != mode) {
            Log.i("HaloReactive", "Output $outputMode -> $mode")
            outputMode = mode
        }
        if (SystemClock.uptimeMillis() < retryAt) return
        try {
            if (remote == null) {
                val candidate = ServiceManager.checkService(SERVICE)
                if (candidate == null) {
                    disconnect()
                    return
                }
                candidate.linkToDeath({
                    handler.post {
                        if (binder === candidate) {
                            disconnect()
                        }
                    }
                }, 0)
                binder = candidate
                remote = ILightRing.Stub.asInterface(candidate)
                lastEffect = null
            }
            if (clearPending) {
                remote!!.clearEffect()
                lastEffect = "off"
                clearPending = false
            }
            val key = effect?.let { "${it.type}:${it.colors.joinToString()}:${it.speed}:${it.brightness}" }
                ?: "off"
            if (lastEffect != key) {
                if (effect == null) remote!!.clearEffect() else remote!!.setEffect(effect)
                lastEffect = key
            }
            connectionChanged(true)
        } catch (exception: Exception) {
            val message = when (exception) {
                is SecurityException -> "Light service denied access"
                is android.os.RemoteException -> "Light service connection lost"
                else -> "Light service could not apply effect"
            }
            Log.w("LegionHalo", message, exception)
            // Clear a partially applied request before restoring manual control on reconnect.
            clearPending = true
            disconnect()
        }
    }

    private fun disconnect() {
        pulses.stop(resetFailure = true)
        session.pauseForHal()
        clearPending = true
        pulseLevel = null
        remote = null
        binder = null
        lastEffect = null
        connectionChanged(false)
        retryAt = SystemClock.uptimeMillis() + 2000
        handler.removeCallbacks(applyState)
        handler.postAtTime(applyState, retryAt)
    }
}
