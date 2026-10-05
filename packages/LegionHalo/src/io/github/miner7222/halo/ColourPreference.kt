// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.MotionEvent
import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat.AccessibilityActionCompat
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder
import java.util.Locale

internal object ColourPalette {
    fun load(): List<Int> {
        val palette = RingController.parseColours(RingController.preferences.getString(
            "colours", RingController.DEFAULT_COLOURS)!!) ?: RingController.parseColours(RingController.DEFAULT_COLOURS)!!
        return List(9) { palette[it % palette.size] }
    }

    fun hex(colour: Int) = String.format(Locale.ROOT, "%06X", colour and 0xffffff)

    fun save(colours: List<Int>) {
        RingController.preferences.edit().putString("colours", colours.joinToString(",") { hex(it) }).apply()
    }
}

internal class ColourPreference(context: Context, colour: Int) : Preference(context) {
    var colour = colour
        set(value) {
            field = value
            updateSwatch()
        }
    var reorderable = true
        set(value) { field = value; notifyChanged() }
    var canMoveUp = false
    var canMoveDown = false
    var onDrag: ((PreferenceViewHolder) -> Unit)? = null
    var onMove: ((Int) -> Boolean)? = null

    init {
        widgetLayoutResource = R.layout.colour_drag_handle
        updateSwatch()
    }

    private fun updateSwatch() {
        val size = (24 * context.resources.displayMetrics.density).toInt()
        val outline = context.obtainStyledAttributes(intArrayOf(android.R.attr.textColorSecondary)).let {
            val colour = it.getColor(0, Color.GRAY)
            it.recycle()
            colour
        }
        icon = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setSize(size, size)
            setColor(this@ColourPreference.colour or (0xff shl 24))
            setStroke((context.resources.displayMetrics.density).toInt().coerceAtLeast(1), outline)
        }
        summary = "#${ColourPalette.hex(colour)}"
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        val handle = holder.findViewById(R.id.colour_drag_handle) ?: return
        handle.visibility = if (reorderable) View.VISIBLE else View.GONE
        handle.contentDescription = context.getString(R.string.reorder_colour, title)
        handle.setOnTouchListener { view, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN && reorderable && isEnabled) {
                onDrag?.invoke(holder)
            }
            if (event.actionMasked == MotionEvent.ACTION_UP) view.performClick()
            true
        }
        ViewCompat.removeAccessibilityAction(handle, AccessibilityActionCompat.ACTION_SCROLL_BACKWARD.id)
        ViewCompat.removeAccessibilityAction(handle, AccessibilityActionCompat.ACTION_SCROLL_FORWARD.id)
        if (reorderable && isEnabled && canMoveUp) ViewCompat.replaceAccessibilityAction(handle,
            AccessibilityActionCompat.ACTION_SCROLL_BACKWARD, context.getString(R.string.move_up)) { _, _ ->
                onMove?.invoke(-1) == true
            }
        if (reorderable && isEnabled && canMoveDown) ViewCompat.replaceAccessibilityAction(handle,
            AccessibilityActionCompat.ACTION_SCROLL_FORWARD, context.getString(R.string.move_down)) { _, _ ->
                onMove?.invoke(1) == true
            }
    }
}
