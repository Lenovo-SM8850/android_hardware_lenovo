// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.Dialog
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.DialogFragment

class ColourPickerDialog : DialogFragment() {
    private lateinit var input: EditText
    private var selectedColour = 0

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val context = requireContext()
        val index = requireArguments().getInt("index")
        selectedColour = savedInstanceState?.getInt("colour") ?: requireArguments().getInt("colour")
        val dp = context.resources.displayMetrics.density
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((24 * dp).toInt(), (8 * dp).toInt(), (24 * dp).toInt(), 0)
        }
        content.addView(TextView(context).apply { setText(R.string.colours_help) })
        val wheel = ColourWheelView(context)
        content.addView(wheel, LinearLayout.LayoutParams(-1, (240 * dp).toInt()).apply {
            topMargin = (16 * dp).toInt()
        })
        // Brightness track: black up to the full-brightness colour of the selected hue and saturation.
        val track = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, intArrayOf(Color.BLACK, Color.WHITE)).apply {
            cornerRadius = 4 * dp
            setSize(-1, (8 * dp).toInt())
        }
        val brightness = SeekBar(context).apply {
            max = 100
            contentDescription = getString(R.string.brightness)
            // The gradient track must not be clipped to progress or tinted by the theme.
            progressTintList = null
            progressBackgroundTintList = null
            secondaryProgressTintList = null
            progressDrawable = track
            splitTrack = false
            thumbTintList = null
            thumb = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setSize((20 * dp).toInt(), (20 * dp).toInt())
                setColor(Color.WHITE)
                setStroke((2 * dp).toInt(), Color.BLACK)
            }
        }
        content.addView(brightness, LinearLayout.LayoutParams(-1, (48 * dp).toInt()))
        input = EditText(context).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            isSingleLine = true
            hint = "#RRGGBB"
            contentDescription = getString(R.string.colour_code)
            setText(savedInstanceState?.getString("text") ?: "#${ColourPalette.hex(selectedColour)}")
        }
        content.addView(input, LinearLayout.LayoutParams(-1, -2))
        var updating = false
        fun updateTrack() {
            track.colors = intArrayOf(Color.BLACK, Color.HSVToColor(floatArrayOf(wheel.hue, wheel.saturation, 1f)))
        }
        fun updateControls(colour: Int) {
            selectedColour = colour
            updating = true
            wheel.setColour(colour)
            brightness.progress = Math.round(wheel.value * 100)
            updateTrack()
            updating = false
        }
        fun updateText(colour: Int) {
            selectedColour = colour
            updating = true
            input.setText("#${ColourPalette.hex(colour)}")
            input.setSelection(input.length())
            input.error = null
            updating = false
        }
        updateControls(selectedColour)
        wheel.onColourChanged = {
            updateTrack()
            updateText(it)
        }
        brightness.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (!updating && fromUser) {
                    wheel.setValue(progress / 100f)
                    updateText(wheel.colour)
                }
            }
            override fun onStartTrackingTouch(bar: SeekBar) {}
            override fun onStopTrackingTouch(bar: SeekBar) {}
        })
        input.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                if (!updating) parseHex(s.toString())?.let { updateControls(it); input.error = null }
            }
            override fun afterTextChanged(s: Editable?) {}
        })
        return AlertDialog.Builder(context)
            .setTitle(getString(R.string.colour_number, index + 1))
            .setView(ScrollView(context).apply { addView(content) })
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok, null)
            .create().apply {
                setOnShowListener {
                    getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                        val colour = parseHex(input.text.toString())
                        if (colour == null) {
                            input.error = getString(R.string.invalid_colours)
                        } else {
                            val palette = ColourPalette.load().toMutableList()
                            palette[index] = colour
                            ColourPalette.save(palette)
                            dismiss()
                        }
                    }
                }
            }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt("colour", selectedColour)
        if (::input.isInitialized) outState.putString("text", input.text.toString())
    }

    private fun parseHex(text: String): Int? {
        val value = text.trim().removePrefix("#")
        return if (value.matches(Regex("[0-9a-fA-F]{6}"))) value.toInt(16) else null
    }

    companion object {
        fun newInstance(index: Int, colour: Int) = ColourPickerDialog().apply {
            arguments = Bundle().apply { putInt("index", index); putInt("colour", colour) }
        }
    }
}
