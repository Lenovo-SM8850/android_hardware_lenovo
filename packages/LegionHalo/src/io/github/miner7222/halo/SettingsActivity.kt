// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.os.Bundle
import android.view.View
import androidx.preference.PreferenceGroupAdapter
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.ItemTouchHelper
import androidx.recyclerview.widget.RecyclerView
import androidx.preference.ListPreference
import androidx.preference.PreferenceCategory
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.android.settingslib.widget.SliderPreference
import com.android.settingslib.widget.TopIntroPreference

class SettingsActivity : CollapsingToolbarBaseActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val frame = com.android.settingslib.collapsingtoolbar.R.id.content_frame
        if (supportFragmentManager.findFragmentById(frame) == null) {
            supportFragmentManager.beginTransaction().add(frame, SettingsFragment()).commit()
        }
    }
}

class SettingsFragment : SettingsBasePreferenceFragment() {
    private val colours = mutableListOf<ColourPreference>()
    private lateinit var colourGroup: PreferenceCategory
    private var dragging = false
    private var multipleColours = true
    private var touchHelper: ItemTouchHelper? = null
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
        screen.addPreference(TopIntroPreference(context).apply {
            setTitle(R.string.availability)
        })
        screen.addPreference(MainSwitchPreference(context).apply {
            key = KEY_ENABLED
            setTitle(R.string.enabled)
            setDefaultValue(false)
        })
        val speed = ListPreference(context).apply {
            key = "speed"
            setTitle(R.string.speed)
            entries = arrayOf(getString(R.string.slow), getString(R.string.normal), getString(R.string.fast))
            entryValues = arrayOf("0", "1", "2")
            setDefaultValue("1")
            summaryProvider = ListPreference.SimpleSummaryProvider.getInstance()
        }
        fun updateSpeed(type: String) {
            speed.isEnabled = type == "1" || type == "3"
            speed.summaryProvider = if (type == "2") null else ListPreference.SimpleSummaryProvider.getInstance()
            if (type == "2") speed.setSummary(R.string.fixed_speed)
            multipleColours = type == "2" || type == "3"
            colours.forEachIndexed { index, colour ->
                colour.isVisible = multipleColours || index == 0
                colour.reorderable = multipleColours
            }
        }
        val effect = ListPreference(context).apply {
            key = "effect"
            setTitle(R.string.effect)
            entries = arrayOf(getString(R.string.static_colour), getString(R.string.breathing),
                getString(R.string.rainbow), getString(R.string.colour_cycle))
            entryValues = arrayOf("0", "1", "2", "3")
            setDefaultValue("2")
            summaryProvider = ListPreference.SimpleSummaryProvider.getInstance()
            setOnPreferenceChangeListener { _, value -> updateSpeed(value as String); true }
        }
        screen.addPreference(effect)
        colourGroup = PreferenceCategory(context).apply {
            setTitle(R.string.colours)
        }
        screen.addPreference(colourGroup)
        ColourPalette.load().forEachIndexed { index, value ->
            val colour = ColourPreference(context, value).apply {
                key = "colour_$index"
                isPersistent = false
                onDrag = { holder -> touchHelper?.startDrag(holder) }
                onMove = { offset -> moveColour(colours.indexOf(this), colours.indexOf(this) + offset) }
                setOnPreferenceClickListener {
                    ColourPickerDialog.newInstance(colours.indexOf(this), this.colour).show(
                        parentFragmentManager, "colour_picker")
                    true
                }
            }
            colours.add(colour)
            colourGroup.addPreference(colour)
        }
        updateColourLabels()
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
        // Dependencies resolve against the hierarchy, so set them after the rows are added.
        listOf(effect, colourGroup, speed, brightness).forEach { it.dependency = KEY_ENABLED }
        updateSpeed(RingController.preferences.getString("effect", "2")!!)
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        touchHelper = ItemTouchHelper(object : ItemTouchHelper.SimpleCallback(
            ItemTouchHelper.UP or ItemTouchHelper.DOWN, 0) {
            override fun isLongPressDragEnabled() = false

            override fun getMovementFlags(recyclerView: RecyclerView, holder: RecyclerView.ViewHolder): Int {
                val item = (recyclerView.adapter as? PreferenceGroupAdapter)?.getItem(holder.bindingAdapterPosition)
                return if (multipleColours && item is ColourPreference && item.isEnabled) {
                    super.getMovementFlags(recyclerView, holder)
                } else {
                    0
                }
            }

            override fun onMove(recyclerView: RecyclerView, source: RecyclerView.ViewHolder,
                target: RecyclerView.ViewHolder): Boolean {
                val adapter = recyclerView.adapter as? PreferenceGroupAdapter ?: return false
                val from = adapter.getItem(source.bindingAdapterPosition) as? ColourPreference ?: return false
                val to = adapter.getItem(target.bindingAdapterPosition) as? ColourPreference ?: return false
                return moveColour(colours.indexOf(from), colours.indexOf(to))
            }

            override fun onSelectedChanged(holder: RecyclerView.ViewHolder?, actionState: Int) {
                super.onSelectedChanged(holder, actionState)
                if (actionState == ItemTouchHelper.ACTION_STATE_DRAG) dragging = true
            }

            override fun clearView(recyclerView: RecyclerView, holder: RecyclerView.ViewHolder) {
                super.clearView(recyclerView, holder)
                dragging = false
                refreshGroupShapes()
            }

            override fun onSwiped(holder: RecyclerView.ViewHolder, direction: Int) {}
        }).also { it.attachToRecyclerView(listView) }
    }

    private fun moveColour(from: Int, to: Int): Boolean {
        if (!multipleColours || from !in colours.indices || to !in colours.indices || from == to) return false
        colours.add(to, colours.removeAt(from))
        // The colour rows keep their own order inside the Colours group.
        colours.forEachIndexed { index, preference -> preference.order = index }
        updateColourLabels()
        ColourPalette.save(colours.map { it.colour })
        refreshGroupShapes()
        return true
    }

    // The grouped card corners depend on each row's position, and the comparison callback keeps
    // moved rows from being rebound. Rebind once nothing is being dragged.
    private fun refreshGroupShapes() {
        listView?.post { if (!dragging) listView?.adapter?.notifyDataSetChanged() }
    }

    private fun updateColourLabels() {
        colours.forEachIndexed { index, preference ->
            preference.title = getString(R.string.colour_number, index + 1)
            preference.canMoveUp = index > 0
            preference.canMoveDown = index < colours.lastIndex
        }
    }

    private fun updateColours() {
        val palette = ColourPalette.load()
        colours.forEachIndexed { index, preference -> preference.colour = palette[index] }
    }

    override fun onStart() {
        super.onStart()
        RingController.preferences.registerOnSharedPreferenceChangeListener(colourListener)
        updateColours()
    }

    override fun onStop() {
        RingController.preferences.unregisterOnSharedPreferenceChangeListener(colourListener)
        super.onStop()
    }

    override fun onDestroyView() {
        touchHelper?.attachToRecyclerView(null)
        touchHelper = null
        super.onDestroyView()
    }
}

private const val KEY_ENABLED = "enabled"
