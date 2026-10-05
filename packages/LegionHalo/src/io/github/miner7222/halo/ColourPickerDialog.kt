// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.Dialog
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.widget.EditText
import android.widget.SeekBar
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.DialogFragment

class ColourPickerDialog : DialogFragment() {
    private lateinit var input: EditText
    private var selectedColour = 0

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val context = requireContext()
        val index = requireArguments().getInt("index")
        selectedColour = savedInstanceState?.getInt("colour") ?: requireArguments().getInt("colour")
        val builder = AlertDialog.Builder(context)
        val content = LayoutInflater.from(builder.context).inflate(R.layout.colour_picker_dialog, null)
        val wheel = content.findViewById<ColourWheelView>(R.id.colour_wheel)
        // Brightness track: black up to the full-brightness colour of the selected hue and saturation.
        val brightness = content.findViewById<SeekBar>(R.id.colour_brightness).apply {
            // The gradient track must not be clipped to progress or tinted by the theme.
            progressTintList = null
            progressBackgroundTintList = null
            secondaryProgressTintList = null
            thumbTintList = null
        }
        val track = brightness.progressDrawable as GradientDrawable
        input = content.findViewById<EditText>(R.id.colour_code).apply {
            setText(savedInstanceState?.getString("text") ?: "#${ColourPalette.hex(selectedColour)}")
        }
        val trackStart = context.getColor(R.color.colour_picker_dark)
        var updating = false
        fun updateTrack() {
            track.colors = intArrayOf(trackStart, Color.HSVToColor(floatArrayOf(wheel.hue, wheel.saturation, 1f)))
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
        return builder
            .setTitle(getString(R.string.colour_number, index + 1))
            .setView(content)
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
