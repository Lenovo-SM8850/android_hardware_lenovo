// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.ClipData
import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.view.DragEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.GridLayout
import android.widget.ImageView
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder
import com.android.settingslib.widget.GroupSectionDividerMixin
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

/**
 * The colour slots as a grid of cards. A divider-style preference has no group background, so each
 * card draws its own. Long-press a card and drop it on another to reorder.
 */
internal class ColourGridPreference(context: Context) : Preference(context), GroupSectionDividerMixin {
    var colours: List<Int> = emptyList()
        set(value) {
            field = value
            notifyChanged()
        }

    /** False for effects that use a single colour; only the first card is shown then. */
    var multiple = true
        set(value) {
            field = value
            notifyChanged()
        }
    var onSelect: ((Int) -> Unit)? = null
    var onMove: ((Int, Int) -> Unit)? = null

    init {
        layoutResource = R.layout.colour_grid
        isSelectable = false
        isPersistent = false
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        val grid = holder.findViewById(R.id.colour_grid) as GridLayout
        grid.removeAllViews()
        val inflater = LayoutInflater.from(context)
        val gap = context.resources.getDimensionPixelSize(R.dimen.colour_card_gap)
        val outline = context.getColor(R.color.settingslib_materialColorOutline)
        val size = context.resources.getDimensionPixelSize(R.dimen.colour_swatch_size)
        val stroke = context.resources.getDimensionPixelSize(R.dimen.colour_swatch_stroke)
        val count = if (multiple) colours.size else minOf(1, colours.size)
        for (index in 0 until count) {
            val colour = colours[index]
            val card = inflater.inflate(R.layout.colour_card, grid, false)
            card.layoutParams = GridLayout.LayoutParams(
                GridLayout.spec(index / COLUMNS), GridLayout.spec(index % COLUMNS, 1f)).apply {
                width = 0
                height = ViewGroup.LayoutParams.WRAP_CONTENT
                setMargins(gap, gap, gap, gap)
            }
            card.findViewById<ImageView>(R.id.colour_swatch).setImageDrawable(GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setSize(size, size)
                setColor(colour or (0xff shl 24))
                setStroke(stroke, outline)
            })
            card.findViewById<TextView>(R.id.colour_label).text = context.getString(R.string.colour_number, index + 1)
            card.findViewById<TextView>(R.id.colour_hex).text = "#${ColourPalette.hex(colour)}"
            bindCard(card, index, count)
            grid.addView(card)
        }
    }

    private fun bindCard(card: View, index: Int, count: Int) {
        val usable = isEnabled
        card.isEnabled = usable
        card.alpha = if (usable) 1f else DISABLED_ALPHA
        card.setOnClickListener { if (usable) onSelect?.invoke(index) }
        if (!usable) return
        if (multiple) {
            card.setOnLongClickListener {
                it.startDragAndDrop(ClipData.newPlainText("", ""), View.DragShadowBuilder(it), index, 0)
            }
            card.setOnDragListener { view, event ->
                val from = event.localState as? Int
                when (event.action) {
                    DragEvent.ACTION_DRAG_STARTED -> from != null
                    DragEvent.ACTION_DRAG_ENTERED -> {
                        if (from != index) view.isActivated = true
                        true
                    }
                    DragEvent.ACTION_DRAG_EXITED, DragEvent.ACTION_DRAG_ENDED -> {
                        view.isActivated = false
                        true
                    }
                    DragEvent.ACTION_DROP -> {
                        view.isActivated = false
                        if (from != null && from != index) onMove?.invoke(from, index)
                        true
                    }
                    else -> true
                }
            }
            if (index > 0) ViewCompat.addAccessibilityAction(card, context.getString(R.string.move_up)) { _, _ ->
                onMove?.invoke(index, index - 1)
                true
            }
            if (index < count - 1) ViewCompat.addAccessibilityAction(card, context.getString(R.string.move_down)) { _, _ ->
                onMove?.invoke(index, index + 1)
                true
            }
        }
    }

    private companion object {
        const val COLUMNS = 3
        const val DISABLED_ALPHA = 0.38f
    }
}
