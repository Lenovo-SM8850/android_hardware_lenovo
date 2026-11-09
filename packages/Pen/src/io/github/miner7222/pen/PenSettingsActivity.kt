/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.app.ActivityManager
import android.os.Bundle
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.android.settingslib.widget.SliderPreference
import com.google.android.material.slider.LabelFormatter

class PenSettingsActivity : CollapsingToolbarBaseActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val frame = com.android.settingslib.collapsingtoolbar.R.id.content_frame
        if (supportFragmentManager.findFragmentById(frame) == null) {
            supportFragmentManager.beginTransaction().add(frame, PenSettingsFragment()).commit()
        }
    }
}

class PenSettingsFragment : SettingsBasePreferenceFragment() {
    private val user get() = ActivityManager.getCurrentUser()

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.setStorageDeviceProtected()
        preferenceManager.sharedPreferencesName = "pen_settings"
        val context = requireContext()
        val screen = preferenceManager.createPreferenceScreen(context)
        preferenceScreen = screen
        val hapticsSupported = context.resources.getBoolean(R.bool.config_penWritingHapticsSupported)
        if (hapticsSupported) {
            screen.addPreference(MainSwitchPreference(context).apply {
                key = PenSettings.WRITING_HAPTICS
                isPersistent = false
                setTitle(R.string.pen_writing_haptics)
                setSummary(R.string.pen_writing_haptics_summary)
                isChecked = PenSettings.writingHaptics(context, user)
                setOnPreferenceChangeListener { _, value ->
                    PenSettings.setWritingHaptics(context, user, value as Boolean)
                    true
                }
            })
            val names = arrayOf(
                getString(R.string.pen_haptic_level_1), getString(R.string.pen_haptic_level_2),
                getString(R.string.pen_haptic_level_3), getString(R.string.pen_haptic_level_4),
                getString(R.string.pen_haptic_level_5),
            )
            val level = SliderPreference(context).apply {
                key = PenSettings.HAPTIC_LEVEL
                isPersistent = false
                setTitle(R.string.pen_haptic_level)
                min = PenSettings.MIN_HAPTIC_LEVEL
                max = PenSettings.MAX_HAPTIC_LEVEL
                sliderIncrement = 1
                setTickVisible(true)
                setTextStart(R.string.pen_haptic_level_1)
                setTextEnd(R.string.pen_haptic_level_5)
                showSliderValue = true
                setLabelFormater(LabelFormatter { value ->
                    names[value.toInt().coerceIn(
                        PenSettings.MIN_HAPTIC_LEVEL, PenSettings.MAX_HAPTIC_LEVEL) -
                        PenSettings.MIN_HAPTIC_LEVEL]
                })
                value = PenSettings.hapticLevel(context, user)
                setOnPreferenceChangeListener { _, newValue ->
                    PenSettings.setHapticLevel(context, user, newValue as Int)
                    true
                }
            }
            screen.addPreference(level)
            level.dependency = PenSettings.WRITING_HAPTICS
        }
    }
}
