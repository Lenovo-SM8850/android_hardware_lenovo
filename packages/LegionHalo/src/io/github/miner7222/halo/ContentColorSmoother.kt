// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.min

/** Time-based smoothing independent of the capture and FFT callback rates. */
class ContentColorSmoother {
    private var channels: DoubleArray? = null
    private var target = 0
    private var lastAt = 0L

    fun clear() { channels = null }

    fun setTarget(color: Int, now: Long) {
        advance(now)
        if (channels == null) channels = DoubleArray(3) { channel(color, it).toDouble() }
        target = color
        lastAt = now
    }

    fun value(fallback: Int, now: Long): Int {
        advance(now)
        val values = channels ?: return fallback
        return (values[0].toInt().coerceIn(0, 255) shl 16) or
                (values[1].toInt().coerceIn(0, 255) shl 8) or values[2].toInt().coerceIn(0, 255)
    }

    fun moving(): Boolean = channels?.let { values ->
        values.indices.any { abs(values[it] - channel(target, it)) > 1.0 }
    } ?: false

    private fun advance(now: Long) {
        val values = channels ?: return
        val fraction = 1 - exp(-(now - lastAt).coerceAtLeast(0) / 400.0)
        for (i in values.indices) {
            values[i] += (channel(target, i) - values[i]) * fraction
            if (abs(values[i] - channel(target, i)) <= 1) values[i] = channel(target, i).toDouble()
        }
        lastAt = now
    }

    companion object {
        private fun channel(color: Int, index: Int) = color shr (16 - index * 8) and 255
        fun usable(color: Int): Boolean {
            if (color !in 0..0xffffff) return false
            val r = channel(color, 0)
            val g = channel(color, 1)
            val b = channel(color, 2)
            val lightness = (max(r, max(g, b)) + min(r, min(g, b))) / 510.0
            return lightness in 0.15..0.90
        }
    }
}
