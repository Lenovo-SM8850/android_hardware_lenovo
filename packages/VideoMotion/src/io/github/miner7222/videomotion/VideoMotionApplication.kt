// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.videomotion

import android.app.Application
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Process
import android.os.SystemProperties
import android.os.UserHandle
import android.service.quicksettings.TileService
import android.util.Log
import java.util.concurrent.Executors

class VideoMotionApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        MotionController.initialize(this)
        if (MotionController.isOwner()) {
            // Runtime registration avoids implicit package broadcast restrictions.
            registerReceiver(
                PackageReceiver(),
                IntentFilter().apply {
                    addAction(Intent.ACTION_PACKAGE_ADDED)
                    addAction(Intent.ACTION_PACKAGE_REPLACED)
                    addAction(Intent.ACTION_PACKAGE_REMOVED)
                    addDataScheme("package")
                },
                Context.RECEIVER_NOT_EXPORTED,
            )
        }
    }
}

object MotionController {
    const val ENABLED = "enabled"
    private const val SELECTED = "selected_packages"
    private const val PUBLISHED = "published_uids"
    private const val PREFIX = "sys.lenovo.memc."
    private const val TAG = "VideoMotion"
    private lateinit var context: Context
    lateinit var preferences: SharedPreferences
        private set

    private val executor = Executors.newSingleThreadExecutor()
    private val listener =
        SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == ENABLED || key == SELECTED) {
                publish()
                TileService.requestListeningState(
                    context,
                    ComponentName(context, VideoMotionTile::class.java),
                )
            }
        }

    // Publish UIDs only for the system user.
    fun isOwner() = UserHandle.getUserId(Process.myUid()) == UserHandle.USER_SYSTEM

    fun enabled() = preferences.getBoolean(ENABLED, false)

    fun selected(): Set<String> = preferences.getStringSet(SELECTED, emptySet())!!.toSet()

    fun initialize(application: Context) {
        context = application.createDeviceProtectedStorageContext()
        preferences = context.getSharedPreferences("video_motion", Context.MODE_PRIVATE)
        if (isOwner()) {
            preferences.registerOnSharedPreferenceChangeListener(listener)
            publish()
        }
    }

    fun setEnabled(value: Boolean) {
        if (isOwner()) preferences.edit().putBoolean(ENABLED, value).apply()
    }

    @Synchronized
    fun setSelected(packageName: String, value: Boolean) {
        if (!isOwner()) return
        val packages = selected().toMutableSet()
        if (value) packages.add(packageName) else packages.remove(packageName)
        preferences.edit().putStringSet(SELECTED, packages).apply()
    }

    fun publish(removedPackage: String? = null, complete: () -> Unit = {}) {
        if (!isOwner()) {
            complete()
            return
        }
        executor.execute {
            try {
                reconcile(removedPackage)
            } catch (e: RuntimeException) {
                Log.e(TAG, "Cannot publish motion smoothing properties", e)
            } finally {
                complete()
            }
        }
    }

    @Synchronized
    private fun reconcile(removedPackage: String?) {
        val packages = selected().toMutableSet()
        if (removedPackage != null) packages.remove(removedPackage)
        val desired = mutableSetOf<String>()
        val active = enabled()
        for (packageName in packages.toList()) {
            try {
                val app = context.packageManager.getApplicationInfo(packageName, 0)
                if (active && UserHandle.getUserId(app.uid) == UserHandle.USER_SYSTEM) {
                    desired.add(app.uid.toString())
                }
            } catch (_: PackageManager.NameNotFoundException) {
                packages.remove(packageName)
            }
        }
        if (packages != selected()) {
            preferences.edit().putStringSet(SELECTED, packages).commit()
        }
        val previous = preferences.getStringSet(PUBLISHED, emptySet())!!.toSet()
        // Journal before setting properties so a killed process can clear stale bits.
        if (!preferences.edit().putStringSet(PUBLISHED, previous + desired).commit()) {
            Log.e(TAG, "Cannot persist the published UID journal")
            return
        }
        for (uid in previous + desired) {
            SystemProperties.set(PREFIX + uid, if (uid in desired) "1" else "0")
        }
        preferences.edit().putStringSet(PUBLISHED, desired).commit()
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (
            intent.action != Intent.ACTION_BOOT_COMPLETED &&
                intent.action != Intent.ACTION_LOCKED_BOOT_COMPLETED
        )
            return
        val pending = goAsync()
        MotionController.publish(complete = { pending.finish() })
    }
}

class PackageReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val uid = intent.getIntExtra(Intent.EXTRA_UID, -1)
        if (uid < 0 || UserHandle.getUserId(uid) != UserHandle.USER_SYSTEM) return
        val packageName = intent.data?.schemeSpecificPart ?: return
        // Do not resolve packages in the temporary removal phase of an update.
        if (
            intent.action == Intent.ACTION_PACKAGE_REMOVED &&
                intent.getBooleanExtra(Intent.EXTRA_REPLACING, false)
        )
            return
        val removed = intent.action == Intent.ACTION_PACKAGE_REMOVED
        // During replacement retain selection, but re-resolve every UID on add/replace.
        val pending = goAsync()
        MotionController.publish(if (removed) packageName else null) { pending.finish() }
    }
}
