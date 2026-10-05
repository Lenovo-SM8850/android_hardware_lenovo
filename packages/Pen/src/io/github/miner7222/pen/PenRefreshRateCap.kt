/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.content.Context
import android.database.ContentObserver
import android.os.Handler
import android.os.ServiceManager
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import vendor.lineage.touch.IHighTouchPollingRate

/** Like OplusPen, use Settings.System; journal exact nullable values before any write. */
internal class PenRefreshRateCap(private val context: Context, private val handler: Handler) {
    private val journal = context.createDeviceProtectedStorageContext()
        .getSharedPreferences("pen_refresh_restore", Context.MODE_PRIVATE)
    private val rate = context.getString(R.string.config_penSupportedRefreshRate).toFloatOrNull()
        ?.takeIf { it.isFinite() && it > 0f }
    private var user = -1
    private var resolver = context.contentResolver
    private var active = false
    private val enforce = object : Runnable {
        override fun run() {
            if (!active) return
            apply()
            if (active) handler.postDelayed(this, 1000)
        }
    }
    private val observer = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) {
            if (active) apply()
        }
    }

    fun recover() {
        if (journal.contains("user")) restore(journal.getInt("user", 0))
    }

    fun activate() {
        if (rate == null || active) return
        if (journal.contains("user")) {
            recover()
            if (journal.contains("user")) return
        }
        if (!active) {
            user = ActivityManager.getCurrentUser()
            resolver = context.createContextAsUser(UserHandle.of(user), 0).contentResolver
            active = true
            for (key in KEYS) {
                resolver.registerContentObserver(Settings.System.getUriFor(key), false, observer)
            }
        }
        apply()
        handler.postDelayed(enforce, 1000)
    }

    fun release() {
        handler.removeCallbacks(enforce)
        if (active) resolver.unregisterContentObserver(observer)
        active = false
        if (journal.contains("user")) restore(journal.getInt("user", 0))
    }

    private fun pollingService(): IHighTouchPollingRate? = IHighTouchPollingRate.Stub.asInterface(
        ServiceManager.checkService(IHighTouchPollingRate.DESCRIPTOR + "/default"))

    private fun apply() {
        val cap = rate ?: return
        if (ActivityManager.getCurrentUser() != user) {
            release()
            return
        }
        try {
            // MIN must also be bounded: DMD uses max(MIN, PEAK) for the peak vote.
            for (key in listOf(Settings.System.MIN_REFRESH_RATE, Settings.System.PEAK_REFRESH_RATE)) {
                val raw = Settings.System.getString(resolver, key)
                val value = raw?.toFloatOrNull()
                val needsCap = if (key == Settings.System.PEAK_REFRESH_RATE) {
                    value == null || !value.isFinite() || value == 0f || value > cap
                } else {
                    value != null && (!value.isFinite() || value > cap)
                }
                updateSavedChoice(key, raw)
                if (needsCap && raw != cap.toString() && save(key, raw, cap.toString())) {
                    check(Settings.System.putString(resolver, key, cap.toString()))
                }
            }
            // Disable high polling through the touch AIDL while the pen is active.
            val polling = pollingService()
            if (polling != null) {
                val enabled = polling.enabled
                val raw = if (enabled) "1" else "0"
                updateSavedChoice("polling", raw)
                if (enabled && save("polling", raw, "0")) {
                    polling.enabled = false
                }
            } else {
                Log.w(TAG, "High touch polling service unavailable")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Cannot apply pen refresh rate cap", e)
        }
    }

    private fun updateSavedChoice(key: String, current: String?) {
        if (journal.contains("applied:$key") && current != journal.getString("applied:$key", null)) {
            // Preserve a setting selected during a pen session, including an absent value.
            val edit = journal.edit().putString("original:$key", current)
                .putString("applied:$key", current)
            if (current == null) edit.remove("applied:$key").remove("original:$key")
            edit.commit()
        }
    }

    private fun save(key: String, original: String?, applied: String): Boolean {
        val edit = journal.edit().putInt("user", user)
        if (!journal.contains("applied:$key")) edit.putString("original:$key", original)
        return edit.putString("applied:$key", applied).commit()
    }

    private fun savedKeys() = journal.all.keys.filter { it.startsWith("applied:") }
        .map { it.removePrefix("applied:") }

    private fun drop(key: String) {
        journal.edit().remove("original:$key").remove("applied:$key").commit()
    }

    private fun restore(savedUser: Int) {
        val cr = context.createContextAsUser(UserHandle.of(savedUser), 0).contentResolver
        for (key in savedKeys()) {
            try {
                val original = journal.getString("original:$key", null)
                val applied = journal.getString("applied:$key", null)
                if (key == "polling") {
                    val polling = pollingService() ?: error("High touch polling service unavailable")
                    val raw = if (polling.enabled) "1" else "0"
                    if (raw == applied) polling.enabled = original == "1"
                } else if (Settings.System.getString(cr, key) == applied) {
                    check(Settings.System.putString(cr, key, original))
                }
                drop(key)
            } catch (e: Exception) {
                // Keep the journal for a later restart instead of silently losing the saved setting.
                Log.e(TAG, "Cannot restore pen refresh rate setting", e)
            }
        }
        if (savedKeys().isEmpty()) journal.edit().clear().commit()
    }

    companion object {
        private const val TAG = "LenovoPenRefreshRate"
        private val KEYS = listOf(Settings.System.PEAK_REFRESH_RATE, Settings.System.MIN_REFRESH_RATE)
    }
}
