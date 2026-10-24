// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

import android.app.ActivityTaskManager
import android.app.TaskInfo
import android.content.ComponentName
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.media.audiofx.Visualizer
import android.os.Handler

fun main() {
    val handler = Handler()
    val levels = mutableListOf<Int?>()
    var errors = 0
    val pulses = AudioPulseWorker(handler, { levels.add(it) }, { errors++ })
    pulses.start()
    handler.advance(250)
    check(levels.any { it != null && it > 0 }) { "Music must produce pulses" }
    Visualizer.captureFails = true
    handler.advance(125)
    check(errors == 1 && pulses.failed) { "Capture failure must release and report once" }
    check(Visualizer.releases == 1) { "Failed native capture must be released" }
    val attempts = Visualizer.attempts
    repeat(10) { pulses.start() }
    check(Visualizer.attempts == attempts) { "Callbacks must not bypass retry backoff" }
    Visualizer.captureFails = false
    handler.advance(2500)
    check(!pulses.failed && Visualizer.attempts == attempts + 1) { "Capture must recover without a settings toggle" }
    Visualizer.silent = true
    handler.advance(2000)
    check(levels.last() == null) { "Silent or uncapturable output must yield to the MCU effect" }
    Visualizer.silent = false
    handler.advance(250)
    check(levels.last() != null && levels.last()!! > 0) { "New audio must resume pulses after silence" }
    Visualizer.captureFails = true
    handler.advance(125)
    pulses.stop(resetFailure = true)
    val stoppedAttempts = Visualizer.attempts
    handler.advance(5000)
    check(Visualizer.attempts == stoppedAttempts) { "Screen off/disable must cancel pending capture retries" }
    Visualizer.captureFails = false
    Visualizer.createFails = true
    pulses.start()
    handler.advance(7000)
    check(Visualizer.attempts in stoppedAttempts + 2..stoppedAttempts + 5) { "Persistent failure retries must stay bounded" }
    Visualizer.createFails = false
    handler.advance(2500)
    check(!pulses.failed) { "Creation failure must also recover" }
    pulses.stop(resetFailure = true)

    val audio = AudioManager()
    val pm = PackageManager()
    pm.apps["game"] = ApplicationInfo(10001, ApplicationInfo.CATEGORY_GAME)
    val context = Context(audio, pm)
    lateinit var session: ReactiveSession
    session = ReactiveSession(context, handler) { session.track(true) }
    ActivityTaskManager.fail = true
    session.track(true)
    check(session.failed) { "Task query failure must be reported" }
    ActivityTaskManager.fail = false
    ActivityTaskManager.task = TaskInfo(0, ComponentName("game"))
    handler.advance(2500)
    check(!session.failed && session.scene(true, true) == "game") { "Tracking must recover without opting out" }
    val media = AudioPlaybackConfiguration(10002, 2, AudioAttributes(1))
    audio.playback(listOf(media))
    check(session.hasAudio(true, true)) { "Music stays eligible during a game" }
    ActivityTaskManager.task = null
    ActivityTaskManager.listener!!.onTaskStackChanged()
    handler.advance(0)
    check(session.scene(true, true) == "media") { "Home must leave the game scene" }
    audio.playback(emptyList())
    check(session.scene(true, true) == "idle") { "Stopped music must restore manual mode" }
    session.track(false)
    check(audio.registers == audio.unregisters && ActivityTaskManager.listener == null)
    ActivityTaskManager.fail = true
    session.track(true)
    session.track(false)
    val registrations = audio.registers
    handler.advance(5000)
    check(audio.registers == registrations) { "Disabled tracking must not resurrect its listeners" }
    println("REACTIVE_LIFECYCLE_TESTS_OK")
}
