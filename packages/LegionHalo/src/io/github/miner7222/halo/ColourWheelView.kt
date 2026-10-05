// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.graphics.SweepGradient
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.sin

/**
 * Colour wheel: the angle selects the hue and the distance from the centre selects the saturation.
 * The brightness (HSV value) is set from outside and only dims the wheel.
 */
internal class ColourWheelView(context: Context) : View(context) {
    private val hsv = floatArrayOf(0f, 1f, 1f)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val density = resources.displayMetrics.density
    private val inset = 14 * density
    private var centreX = 0f
    private var centreY = 0f
    private var radius = 0f
    private var sweep: SweepGradient? = null
    private var fade: RadialGradient? = null
    var onColourChanged: ((Int) -> Unit)? = null
    val hue: Float get() = hsv[0]
    val saturation: Float get() = hsv[1]
    val value: Float get() = hsv[2]
    val colour: Int get() = Color.HSVToColor(hsv) and 0xffffff

    init {
        isFocusable = true
        isClickable = true
        updateDescription()
    }

    fun setColour(colour: Int) {
        // Keep the selected hue when entering an achromatic hex value.
        val previousHue = hsv[0]
        Color.colorToHSV(colour or (0xff shl 24), hsv)
        if (hsv[1] == 0f) hsv[0] = previousHue
        updateDescription()
        invalidate()
    }

    fun setValue(value: Float) {
        hsv[2] = value.coerceIn(0f, 1f)
        updateDescription()
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        centreX = w / 2f
        centreY = h / 2f
        radius = (min(w, h) / 2f - inset).coerceAtLeast(1f)
        sweep = SweepGradient(centreX, centreY,
            intArrayOf(Color.RED, Color.YELLOW, Color.GREEN, Color.CYAN, Color.BLUE, Color.MAGENTA, Color.RED),
            null)
        fade = RadialGradient(centreX, centreY, radius, Color.WHITE, 0x00ffffff, Shader.TileMode.CLAMP)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        paint.style = Paint.Style.FILL
        paint.shader = sweep
        canvas.drawCircle(centreX, centreY, radius, paint)
        paint.shader = fade
        canvas.drawCircle(centreX, centreY, radius, paint)
        paint.shader = null
        // Dim the wheel to the selected brightness.
        paint.color = Color.argb(((1f - hsv[2]) * 255).toInt(), 0, 0, 0)
        canvas.drawCircle(centreX, centreY, radius, paint)

        val angle = Math.toRadians(hsv[0].toDouble())
        val distance = hsv[1] * radius
        val x = centreX + (cos(angle) * distance).toFloat()
        val y = centreY + (sin(angle) * distance).toFloat()
        paint.color = Color.HSVToColor(hsv)
        canvas.drawCircle(x, y, 10 * density, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 3 * density
        paint.color = Color.WHITE
        canvas.drawCircle(x, y, 10 * density, paint)
        paint.strokeWidth = density
        paint.color = Color.BLACK
        canvas.drawCircle(x, y, 11.5f * density, paint)
        paint.style = Paint.Style.FILL
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP -> {
                parent.requestDisallowInterceptTouchEvent(event.actionMasked != MotionEvent.ACTION_UP)
                val dx = event.x - centreX
                val dy = event.y - centreY
                val distance = hypot(dx, dy)
                // The centre has no hue, so keep the current one there.
                if (distance > 1f) {
                    hsv[0] = ((Math.toDegrees(atan2(dy, dx).toDouble()) + 360.0) % 360.0).toFloat()
                }
                hsv[1] = (distance / radius).coerceIn(0f, 1f)
                changed()
                if (event.actionMasked == MotionEvent.ACTION_UP) performClick()
                return true
            }
            MotionEvent.ACTION_CANCEL -> parent.requestDisallowInterceptTouchEvent(false)
        }
        return super.onTouchEvent(event)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_RIGHT -> hsv[0] = (hsv[0] + 5f) % 360f
            KeyEvent.KEYCODE_DPAD_LEFT -> hsv[0] = (hsv[0] + 355f) % 360f
            KeyEvent.KEYCODE_DPAD_UP -> hsv[1] = (hsv[1] + .05f).coerceAtMost(1f)
            KeyEvent.KEYCODE_DPAD_DOWN -> hsv[1] = (hsv[1] - .05f).coerceAtLeast(0f)
            else -> return super.onKeyDown(keyCode, event)
        }
        changed()
        return true
    }

    override fun performClick(): Boolean { super.performClick(); return true }

    private fun changed() {
        updateDescription()
        invalidate()
        onColourChanged?.invoke(colour)
    }

    private fun updateDescription() {
        contentDescription = "${context.getString(R.string.hue)} ${hsv[0].toInt()}, " +
            context.getString(R.string.saturation_value, (hsv[1] * 100).toInt(), (hsv[2] * 100).toInt())
    }
}
