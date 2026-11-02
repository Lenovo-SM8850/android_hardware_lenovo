// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.res.Resources
import android.os.Bundle
import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.os.UserHandle
import android.provider.Settings
import android.view.View
import androidx.preference.PreferenceManager
import androidx.preference.ListPreference
import androidx.preference.PreferenceCategory
import androidx.preference.SwitchPreferenceCompat
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity
import com.android.settingslib.widget.FooterPreference
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.android.settingslib.widget.SliderPreference
import com.android.settingslib.widget.UntitledPreferenceCategory
import com.google.android.material.slider.LabelFormatter

class SettingsActivity : CollapsingToolbarBaseActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ContentWidth.alignTitle(this, expandInLandscape = savedInstanceState == null)
        val frame = com.android.settingslib.collapsingtoolbar.R.id.content_frame
        if (supportFragmentManager.findFragmentById(frame) == null) {
            supportFragmentManager.beginTransaction().add(frame, SettingsFragment()).commit()
        }
    }

    // The base class reapplies its own theme on every call, so the dialog overlay goes on last.
    override fun getTheme(): Resources.Theme =
        super.getTheme().also { it.applyStyle(R.style.ThemeOverlay_Halo, true) }
}

class SettingsFragment : SettingsBasePreferenceFragment() {
    private lateinit var colourGrid: ColourGridPreference
    private lateinit var reactive: SwitchPreferenceCompat
    private val reactiveObserver = object : ContentObserver(Handler(Looper.getMainLooper())) {
        override fun onChange(selfChange: Boolean) {
            reactive.isChecked = ReactiveSettings.enabled(requireContext())
        }
    }
    private val colourListener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == "colours") updateColours()
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.setStorageDeviceProtected()
        preferenceManager.sharedPreferencesName = "light_ring"
        preferenceManager.preferenceComparisonCallback = PreferenceManager.SimplePreferenceComparisonCallback()
        val context = requireContext()
        val screen = preferenceManager.createPreferenceScreen(context)
        preferenceScreen = screen
        screen.addPreference(MainSwitchPreference(context).apply {
            key = KEY_ENABLED
            setTitle(R.string.enabled)
            setDefaultValue(false)
        })
        // The speed is stored as the same "0".."2" string the ring controller reads.
        val speedNames = arrayOf(getString(R.string.slow), getString(R.string.normal), getString(R.string.fast))
        val speed = SliderPreference(context).apply {
            key = "speed"
            isPersistent = false
            setTitle(R.string.speed)
            min = 0
            max = 2
            sliderIncrement = 1
            setTickVisible(true)
            setTextStart(R.string.slow)
            setTextEnd(R.string.fast)
            showSliderValue = true
            setLabelFormater(LabelFormatter { value -> speedNames[value.toInt().coerceIn(0, 2)] })
            value = RingController.preferences.getString("speed", "1")!!.toInt().coerceIn(0, 2)
            setOnPreferenceChangeListener { _, newValue ->
                RingController.preferences.edit().putString("speed", (newValue as Int).toString()).apply()
                true
            }
        }
        colourGrid = ColourGridPreference(context).apply {
            key = "colours_grid"
            colours = ColourPalette.load()
            onSelect = { index ->
                ColourPickerDialog.newInstance(index, ColourPalette.load()[index]).show(
                    parentFragmentManager, "colour_picker")
            }
            onMove = { from, to -> moveColour(from, to) }
        }
        fun updateSpeed(type: String) {
            // Rainbow rotation has a fixed speed, so its slider stays visible but disabled.
            speed.isEnabled = type == "1" || type == "3"
            colourGrid.multiple = type == "2" || type == "3"
        }
        val effect = ListPreference(context).apply {
            key = "effect"
            setTitle(R.string.effect)
            setDialogTitle(R.string.effect)
            entries = arrayOf(getString(R.string.static_colour), getString(R.string.breathing),
                getString(R.string.rainbow), getString(R.string.colour_cycle))
            entryValues = arrayOf("0", "1", "2", "3")
            setDefaultValue("2")
            summaryProvider = ListPreference.SimpleSummaryProvider.getInstance()
            setOnPreferenceChangeListener { _, value -> updateSpeed(value as String); true }
        }
        screen.addPreference(effect)
        val colourGroup = PreferenceCategory(context).apply {
            setTitle(R.string.colours)
        }
        screen.addPreference(colourGroup)
        colourGroup.addPreference(colourGrid)
        screen.addPreference(speed)
        val brightness = SliderPreference(context).apply {
            key = "brightness"
            setTitle(R.string.brightness)
            min = 0
            max = 255
            sliderIncrement = 1
            showSliderValue = true
            setDefaultValue(128)
        }
        screen.addPreference(brightness)
        reactive = SwitchPreferenceCompat(context).apply {
            key = ReactiveSettings.ENABLED
            isPersistent = false
            setTitle(R.string.react_enabled)
            isChecked = ReactiveSettings.enabled(context)
            isEnabled = UserHandle.myUserId() == UserHandle.USER_SYSTEM
            setOnPreferenceChangeListener { _, value ->
                ReactiveSettings.setEnabled(context, value as Boolean)
            }
        }
        screen.addPreference(reactive)
        // The options sit in their own card below the switch that governs them.
        val reactiveOptions = UntitledPreferenceCategory(context)
        screen.addPreference(reactiveOptions)
        listOf(
            "react_music" to R.string.react_music,
            "react_games" to R.string.react_games,
            "react_rhythm" to R.string.react_rhythm,
        ).forEach { (setting, title) ->
            val option = SwitchPreferenceCompat(context).apply {
                key = setting
                setTitle(title)
                setDefaultValue(true)
            }
            reactiveOptions.addPreference(option)
            option.dependency = ReactiveSettings.ENABLED
        }
        screen.addPreference(FooterPreference(context).apply {
            title = getString(R.string.availability)
        })
        // Dependencies resolve against the hierarchy, so set them after the rows are added.
        listOf(effect, colourGroup, speed, brightness).forEach { it.dependency = KEY_ENABLED }
        updateSpeed(RingController.preferences.getString("effect", "2")!!)
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        listView?.let { ContentWidth.constrain(it) }
    }

    // Moving a colour keeps the slots in place and shifts the colours between them.
    private fun moveColour(from: Int, to: Int) {
        val palette = ColourPalette.load().toMutableList()
        if (!colourGrid.multiple || from !in palette.indices || to !in palette.indices || from == to) return
        palette.add(to, palette.removeAt(from))
        ColourPalette.save(palette)
    }

    private fun updateColours() {
        colourGrid.colours = ColourPalette.load()
    }

    override fun onStart() {
        super.onStart()
        requireContext().contentResolver.registerContentObserver(
            Settings.Secure.getUriFor(ReactiveSettings.ENABLED), false,
            reactiveObserver, UserHandle.USER_SYSTEM
        )
        reactive.isChecked = ReactiveSettings.enabled(requireContext())
        RingController.preferences.registerOnSharedPreferenceChangeListener(colourListener)
        updateColours()
    }

    override fun onStop() {
        requireContext().contentResolver.unregisterContentObserver(reactiveObserver)
        RingController.preferences.unregisterOnSharedPreferenceChangeListener(colourListener)
        super.onStop()
    }
}

private const val KEY_ENABLED = "enabled"
