/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.content.Context
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.ServiceManager
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import android.view.Display
import org.evolution.display.RefreshRateManager
import vendor.lineage.touch.IHighTouchPollingRate

internal class PenRefreshRateCap(private val context: Context, private val handler: Handler) {
    private val displayManager = context.getSystemService(DisplayManager::class.java)
    private val journal = context.createDeviceProtectedStorageContext()
        .getSharedPreferences("pen_refresh_restore", Context.MODE_PRIVATE)
    private val rate = context.getString(R.string.config_penSupportedRefreshRate).toFloatOrNull()
        ?.takeIf { it.isFinite() && it > 0f }
    private val manager by lazy {
        // EvoX's helper needs a display-associated context, unlike a plain Service context.
        val display = displayManager.getDisplay(Display.DEFAULT_DISPLAY)
        display?.let { context.createDisplayContext(it).getSystemService(RefreshRateManager::class.java) }
    }
    private var user = -1
    private var active = false
    private var pollingActive = false
    private var observing = false
    private var lastRequestedRate = 0f
    private var requestedRefresh = false
    private val enforce = object : Runnable {
        override fun run() {
            if (!active && !pollingActive) return
            apply()
            if (active || pollingActive) handler.postDelayed(this, 1000)
        }
    }
    private val observer = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) {
            if (active || pollingActive) apply()
        }
    }

    fun recover() {
        if (!observing) {
            context.contentResolver.registerContentObserver(
                Settings.Secure.getUriFor(REFRESH_RATE_LIMIT), false, observer, UserHandle.USER_ALL)
            for (key in EVOX_KEYS) {
                context.contentResolver.registerContentObserver(
                    Settings.System.getUriFor(key), false, observer, UserHandle.USER_ALL)
            }
            observing = true
        }
        if (journal.contains("user")) restore(journal.getInt("user", 0))
    }

    fun destroy() {
        if (requestedRefresh) requestRefreshRate(0f, force = true)
        release()
        if (observing) context.contentResolver.unregisterContentObserver(observer)
        observing = false
    }

    fun activate() {
        activatePolling()
        if (!pollingActive || active) return
        active = true
        apply()
    }

    fun activatePolling() {
        if (rate == null || pollingActive) return
        if (journal.contains("user")) {
            recover()
            if (journal.contains("user")) return
        }
        user = ActivityManager.getCurrentUser()
        pollingActive = true
        apply()
        handler.postDelayed(enforce, 1000)
    }

    fun releaseRefresh() {
        active = false
        applyRefresh()
        if (journal.contains("user")) restore(journal.getInt("user", 0), refreshOnly = true)
    }

    fun release() {
        handler.removeCallbacks(enforce)
        active = false
        pollingActive = false
        applyRefresh()
        if (journal.contains("user")) restore(journal.getInt("user", 0))
    }

    private fun pollingService(): IHighTouchPollingRate? = IHighTouchPollingRate.Stub.asInterface(
        ServiceManager.checkService(IHighTouchPollingRate.DESCRIPTOR + "/default"))

    private fun applyRefresh() {
        try {
            requestRefreshRate(
                if (active && limitEnabled() && ActivityManager.getCurrentUser() == user) rate ?: 0f else 0f)
        } catch (e: Exception) {
            Log.e(TAG, "Cannot apply pen refresh rate cap", e)
        }
    }

    private fun limitEnabled() = Settings.Secure.getIntForUser(context.contentResolver,
        REFRESH_RATE_LIMIT, 1, ActivityManager.getCurrentUser()) != 0

    private fun requestRefreshRate(value: Float, force: Boolean = false) {
        if (!force && value == lastRequestedRate) return
        try {
            displayManager.requestMaxRefreshRate(Display.DEFAULT_DISPLAY, value)
            lastRequestedRate = value
            requestedRefresh = true
        } catch (e: Exception) {
            Log.e(TAG, "Cannot apply pen refresh rate cap", e)
        }
    }

    private fun apply() {
        val cap = rate ?: return
        if (ActivityManager.getCurrentUser() != user) {
            release()
            return
        }
        applyRefresh()
        try {
            val resolver = context.createContextAsUser(UserHandle.of(user), 0).contentResolver
            if (active && limitEnabled()) {
                // EvoX votes for extreme/per-app rates at priority 24, above the display request (18).
                // Its manager must update the cached policy too; changing the string alone is insufficient.
                val extreme = Settings.System.getString(resolver, Settings.System.EXTREME_REFRESH_RATE)
                updateSavedChoice(Settings.System.EXTREME_REFRESH_RATE, extreme)
                if (extreme == "1" && manager != null &&
                    save(Settings.System.EXTREME_REFRESH_RATE, extreme, "0")) {
                    manager?.setExtremeRefreshRateEnabled(false)
                }
                val config = Settings.System.getString(resolver, Settings.System.REFRESH_RATE_CONFIG_CUSTOM)
                val entries = parseConfig(config)
                for ((pkg, value) in entries) {
                    val key = "app:$pkg"
                    updateSavedChoice(key, value.toString())
                    if (value > cap && manager != null && save(key, value.toString(), cap.toInt().toString())) {
                        manager?.setRefreshRateForPackage(pkg, cap.toInt())
                    }
                }
                // Removing a custom app entry is a user choice, not a reason to resurrect it.
                for (key in savedKeys().filter { it.startsWith("app:") }) {
                    if (!entries.containsKey(key.removePrefix("app:"))) drop(key)
                }
            } else if (savedKeys().any { it != "polling" }) {
                // The limit was switched off or released: give the overridden rates back.
                restore(user, refreshOnly = true)
            }
            // Keep high polling disabled while the pen session remains ready.
            if (pollingActive) {
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

    private fun restore(savedUser: Int, refreshOnly: Boolean = false) {
        val cr = context.createContextAsUser(UserHandle.of(savedUser), 0).contentResolver
        val currentUser = ActivityManager.getCurrentUser() == savedUser
        for (key in savedKeys().filter { !refreshOnly || it != "polling" }) {
            try {
                val original = journal.getString("original:$key", null)
                val applied = journal.getString("applied:$key", null)
                if (key == "polling") {
                    val polling = pollingService() ?: error("High touch polling service unavailable")
                    val raw = if (polling.enabled) "1" else "0"
                    if (raw == applied) polling.enabled = original == "1"
                } else if (key.startsWith("app:")) {
                    val pkg = key.removePrefix("app:")
                    val config = parseConfig(Settings.System.getString(cr, Settings.System.REFRESH_RATE_CONFIG_CUSTOM))
                    if (config[pkg]?.toString() == applied && original != null) {
                        if (currentUser && manager != null) {
                            manager?.setRefreshRateForPackage(pkg, original.toInt())
                        } else {
                            config[pkg] = original.toInt()
                            check(Settings.System.putString(cr, Settings.System.REFRESH_RATE_CONFIG_CUSTOM,
                                config.entries.joinToString(";") { "${it.key},${it.value}" } + ";"))
                        }
                    }
                } else if (Settings.System.getString(cr, key) == applied) {
                    if (key == Settings.System.EXTREME_REFRESH_RATE && currentUser && manager != null) {
                        manager?.setExtremeRefreshRateEnabled(original == "1")
                    }
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

    private fun parseConfig(raw: String?): LinkedHashMap<String, Int> {
        val entries = linkedMapOf<String, Int>()
        raw?.split(';')?.forEach {
            val fields = it.split(',')
            if (fields.size == 2) fields[1].toIntOrNull()?.let { rate -> entries[fields[0]] = rate }
        }
        return entries
    }

    companion object {
        private const val TAG = "LenovoPenRefreshRate"
        // The Settings stylus page writes this per-user switch.
        private const val REFRESH_RATE_LIMIT = "pen_refresh_rate_limit"
        private val EVOX_KEYS = listOf(Settings.System.EXTREME_REFRESH_RATE,
            Settings.System.REFRESH_RATE_CONFIG_CUSTOM)
    }
}
