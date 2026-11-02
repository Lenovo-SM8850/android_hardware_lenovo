// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.Dialog
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.Editable
import android.text.InputFilter
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
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
        val builder = AlertDialog.Builder(context)
        // Landscape and short windows use two columns so the keyboard does not cut the dialog.
        val configuration = context.resources.configuration
        val wide = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE ||
            configuration.screenHeightDp < WIDE_BELOW_HEIGHT_DP
        val content = LayoutInflater.from(builder.context).inflate(
            if (wide) R.layout.colour_picker_dialog_wide else R.layout.colour_picker_dialog, null)
        val title = getString(R.string.colour_number, index + 1)
        content.findViewById<TextView>(R.id.colour_title).text = title
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
        val error = content.findViewById<View>(R.id.colour_code_error)
        val ok = content.findViewById<Button>(R.id.colour_ok)
        input = content.findViewById<EditText>(R.id.colour_code).apply {
            filters = arrayOf(HexFilter, InputFilter.LengthFilter(HEX_LENGTH))
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
            updating = false
        }
        // An invalid code shows the inline error and disables OK.
        fun validate(text: String) {
            val valid = parseHex(text) != null
            input.isActivated = !valid
            error.visibility = if (valid) View.INVISIBLE else View.VISIBLE
            ok.isEnabled = valid
        }
        updateControls(selectedColour)
        validate(input.text.toString())
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
                val text = s.toString()
                if (!updating) parseHex(text)?.let { updateControls(it) }
                validate(text)
            }
            override fun afterTextChanged(s: Editable?) {}
        })
        ok.setOnClickListener {
            val colour = parseHex(input.text.toString()) ?: return@setOnClickListener
            val palette = ColourPalette.load().toMutableList()
            palette[index] = colour
            ColourPalette.save(palette)
            dismiss()
        }
        content.findViewById<Button>(R.id.colour_cancel).setOnClickListener { dismiss() }
        input.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                if (ok.isEnabled) ok.performClick()
                true
            } else {
                false
            }
        }
        return builder.setView(content).create().apply {
            // The title is part of the content, so the window title only serves accessibility.
            window?.setTitle(title)
            window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
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
        private const val HEX_LENGTH = 7
        private const val WIDE_BELOW_HEIGHT_DP = 480

        // Keeps only "#" and hex digits, in upper case.
        private val HexFilter = InputFilter { source, start, end, _, _, _ ->
            val kept = StringBuilder()
            for (i in start until end) {
                val c = source[i]
                if (c == '#' || c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F') kept.append(c.uppercaseChar())
            }
            if (kept.length == end - start && kept.toString() == source.subSequence(start, end).toString()) null else kept
        }

        fun newInstance(index: Int, colour: Int) = ColourPickerDialog().apply {
            arguments = Bundle().apply { putInt("index", index); putInt("colour", colour) }
        }
    }
}
