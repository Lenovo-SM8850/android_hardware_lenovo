/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.content.Context
import android.provider.Settings

/** Per-user pen preferences. Stock stores these in Global; our keys stay in Secure. */
internal object PenSettings {
    const val WRITING_HAPTICS = "pen_writing_haptics_enabled"
    const val HAPTIC_LEVEL = "pen_writing_haptics_level"
    const val DEFAULT_HAPTIC_LEVEL = 3
    const val MIN_HAPTIC_LEVEL = 1
    const val MAX_HAPTIC_LEVEL = 5

    fun user() = ActivityManager.getCurrentUser()

    fun writingHaptics(context: Context, user: Int = user()): Boolean =
        getInt(context, WRITING_HAPTICS, 1, user) != 0

    fun hapticLevel(context: Context, user: Int = user()): Int =
        getInt(context, HAPTIC_LEVEL, DEFAULT_HAPTIC_LEVEL, user)
            .coerceIn(MIN_HAPTIC_LEVEL, MAX_HAPTIC_LEVEL)

    fun setWritingHaptics(context: Context, user: Int, enabled: Boolean) =
        putInt(context, WRITING_HAPTICS, if (enabled) 1 else 0, user)

    fun setHapticLevel(context: Context, user: Int, level: Int) =
        putInt(context, HAPTIC_LEVEL, level.coerceIn(MIN_HAPTIC_LEVEL, MAX_HAPTIC_LEVEL), user)

    /**
     * The haptics toggle used to default to off. Only users that never stored a value move to the
     * stock on-by-default; an explicit 0 from the old Settings toggle is left alone.
     */
    fun migrate(context: Context, user: Int) {
        try {
            if (Settings.Secure.getStringForUser(context.contentResolver, WRITING_HAPTICS, user) == null) {
                Settings.Secure.putIntForUser(context.contentResolver, WRITING_HAPTICS, 1, user)
            }
        } catch (e: RuntimeException) {
            android.util.Log.w("PenSettings", "Cannot migrate pen haptics default", e)
        }
    }

    private fun getInt(context: Context, key: String, def: Int, user: Int): Int = try {
        Settings.Secure.getIntForUser(context.contentResolver, key, def, user)
    } catch (e: RuntimeException) {
        android.util.Log.w("PenSettings", "Cannot read $key", e)
        def
    }

    private fun putInt(context: Context, key: String, value: Int, user: Int) {
        try {
            Settings.Secure.putIntForUser(context.contentResolver, key, value, user)
        } catch (e: RuntimeException) {
            android.util.Log.w("PenSettings", "Cannot write $key", e)
        }
    }
}
