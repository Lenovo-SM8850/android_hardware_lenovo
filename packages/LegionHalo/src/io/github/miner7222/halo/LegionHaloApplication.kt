// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.Application
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.os.RemoteException
import android.os.ServiceManager
import android.service.quicksettings.TileService
import android.util.Log
import vendor.lenovo.hardware.lightring.Effect
import vendor.lenovo.hardware.lightring.ILightRing

class LegionHaloApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        RingController.initialize(this)
    }
}

object RingController {
    const val DEFAULT_COLOURS = "FF0000,FF8000,FFFF00,00FF00,00FFFF,0000FF,8000FF,FF00FF,FFFFFF"
    private const val SERVICE = "vendor.lenovo.hardware.lightring.ILightRing/default"
    private lateinit var context: Context
    lateinit var preferences: SharedPreferences
        private set
    private lateinit var handler: Handler
    private var remote: ILightRing? = null
    private var binder: IBinder? = null
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
        handler = Handler(HandlerThread("LegionHaloBinder").apply { start() }.looper)
        preferences.registerOnSharedPreferenceChangeListener(preferenceListener)
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_SCREEN_OFF)
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
        return power.isInteractive && !power.isPowerSaveMode
    }
    fun refresh() {
        handler.removeCallbacks(applyState)
        handler.post(applyState)
    }
    private fun updateTile() {
        TileService.requestListeningState(context, ComponentName(context, LegionHaloTile::class.java))
    }
    private fun connectionChanged(value: Boolean) {
        if (connected != value) {
            connected = value
            updateTile()
        }
    }
    private fun apply() {
        try {
            if (remote == null) {
                val candidate = ServiceManager.checkService(SERVICE)
                if (candidate == null) {
                    connectionChanged(false)
                    handler.postDelayed(applyState, 2000)
                    return
                }
                candidate.linkToDeath({
                    handler.post {
                        if (binder === candidate) {
                            binder = null
                            remote = null
                            connectionChanged(false)
                            refresh()
                        }
                    }
                }, 0)
                binder = candidate
                remote = ILightRing.Stub.asInterface(candidate)
            }
            if (enabled() && allowed()) {
                val effect = Effect().apply {
                    type = preferences.getString("effect", "2")!!.toInt().coerceIn(0, 3)
                    val palette = parseColours(preferences.getString("colours", DEFAULT_COLOURS)!!)
                        ?: parseColours(DEFAULT_COLOURS)!!
                    colors = if (type < 2) intArrayOf(palette[0]) else palette
                    speed = preferences.getString("speed", "1")!!.toInt().coerceIn(0, 2)
                    brightness = preferences.getInt("brightness", 128).coerceIn(0, 255)
                }
                remote!!.setEffect(effect)
            } else {
                remote!!.clearEffect()
            }
            connectionChanged(true)
        } catch (exception: SecurityException) {
            Log.w("LegionHalo", "Light service denied access", exception)
            connectionChanged(false)
            handler.postDelayed(applyState, 2000)
        } catch (exception: RemoteException) {
            Log.w("LegionHalo", "Light service connection lost", exception)
            remote = null
            binder = null
            connectionChanged(false)
            handler.postDelayed(applyState, 2000)
        } catch (exception: android.os.ServiceSpecificException) {
            Log.w("LegionHalo", "Light service could not apply effect", exception)
            connectionChanged(false)
            handler.postDelayed(applyState, 2000)
        }
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        RingController.initialize(context.applicationContext)
        RingController.refresh()
    }
}
