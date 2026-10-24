/*
 * Copyright (C) 2022 By yours truly, Daniel Jacob Chittoor
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.halo

import android.media.audiofx.Visualizer
import android.os.Handler
import android.os.SystemClock
import android.util.Log
import kotlin.math.max
import kotlin.math.sqrt

// Output-mix setup and release follow the upstream music visualizer service.
// Polling the FFT on this looper bounds both capture and Binder work to eight frames/s.
class AudioPulseWorker(
    private val handler: Handler,
    private val frame: (Int?) -> Unit,
    private val error: () -> Unit,
) {
    private var visualizer: Visualizer? = null
    private var fft = ByteArray(0)
    private var envelope = 0.0
    private var lastLevel: Int? = null
    private var lastSignal = 0L
    private var started = 0L
    private var samples = 0
    private var frames = 0
    private var minimumLevel = 15
    private var maximumLevel = 0
    var failed = false
        private set
    private val retry = Runnable {
        failed = false
        start()
    }

    private val sample = object : Runnable {
        override fun run() {
            val capture = visualizer ?: return
            val next = SystemClock.uptimeMillis() + FRAME_MS
            try {
                val status = capture.getFft(fft)
                if (status != Visualizer.SUCCESS) Log.w("HaloReactive", "FFT capture status=$status")
                check(status == Visualizer.SUCCESS)
                val rateHz = capture.samplingRate / 1000.0
                var total = 0.0
                var low = 0.0
                var lowBins = 0
                for (bin in 1 until fft.size / 2) {
                    val real = fft[2 * bin].toInt()
                    val imaginary = fft[2 * bin + 1].toInt()
                    val energy = (real * real + imaginary * imaginary).toDouble()
                    total += energy
                    if (bin * rateHz / fft.size <= 250.0) {
                        low += energy
                        lowBins++
                    }
                }
                val rms = sqrt(total / max(1, fft.size / 2 - 1)) / 128.0
                val bass = sqrt(low / max(1, lowBins)) / 128.0
                val target = (max(rms * 3.0, bass * 0.75)).coerceIn(0.0, 1.0)
                envelope = max(target, envelope * 0.6)
                // Sixteen levels prevent insignificant energy changes from uploading a program.
                val level = (envelope * 15.0).toInt().coerceIn(0, 15)
                if (level > 0) lastSignal = SystemClock.uptimeMillis()
                // Keep an automatic effect visible when a player is silent or cannot be captured.
                val output = if (level == 0 && (lastLevel == null ||
                        SystemClock.uptimeMillis() - lastSignal >= SILENCE_MS)) null else level
                minimumLevel = minOf(minimumLevel, level)
                maximumLevel = maxOf(maximumLevel, level)
                samples++
                if (output != lastLevel) {
                    if (output == null) Log.i("HaloReactive", "Output silent; using automatic effect")
                    lastLevel = output
                    frames++
                    frame(output)
                }
                if (visualizer === capture) handler.postAtTime(this, next)
            } catch (exception: RuntimeException) {
                Log.w("HaloReactive", "Output capture stopped", exception)
                stop()
                failed = true
                error()
                handler.postDelayed(retry, RETRY_MS)
            }
        }
    }

    fun start() {
        if (visualizer != null || failed) return
        try {
            // Session zero captures the output mix; it never opens an audio input.
            val capture = Visualizer(0)
            visualizer = capture
            started = SystemClock.elapsedRealtime()
            check(capture.setEnabled(false) == Visualizer.SUCCESS)
            val range = Visualizer.getCaptureSizeRange()
            check(capture.setCaptureSize(1024.coerceIn(range[0], range[1])) == Visualizer.SUCCESS)
            fft = ByteArray(capture.captureSize)
            check(capture.setScalingMode(Visualizer.SCALING_MODE_NORMALIZED) == Visualizer.SUCCESS)
            check(capture.setEnabled(true) == Visualizer.SUCCESS)
            started = SystemClock.elapsedRealtime()
            samples = 0
            frames = 0
            minimumLevel = 15
            maximumLevel = 0
            envelope = 0.0
            lastLevel = null
            lastSignal = SystemClock.uptimeMillis()
            Log.i("HaloReactive", "Output capture started intervalMs=$FRAME_MS")
            handler.post(sample)
        } catch (exception: RuntimeException) {
            Log.w("HaloReactive", "Output capture unavailable", exception)
            stop()
            failed = true
            error()
            handler.postDelayed(retry, RETRY_MS)
        }
    }

    fun stop(resetFailure: Boolean = false) {
        handler.removeCallbacks(sample)
        handler.removeCallbacks(retry)
        val capture = visualizer
        visualizer = null
        if (capture != null) {
            try {
                try {
                    capture.setEnabled(false)
                } finally {
                    capture.release()
                }
            } catch (exception: RuntimeException) {
                Log.w("HaloReactive", "Output capture release failed", exception)
            }
            Log.i("HaloReactive", "Output capture released samples=$samples frames=$frames " +
                "elapsedMs=${SystemClock.elapsedRealtime() - started} " +
                "levels=$minimumLevel..$maximumLevel")
        }
        if (resetFailure) failed = false
    }

    companion object {
        private const val FRAME_MS = 125L
        private const val RETRY_MS = 2000L
        private const val SILENCE_MS = 1000L
    }
}
