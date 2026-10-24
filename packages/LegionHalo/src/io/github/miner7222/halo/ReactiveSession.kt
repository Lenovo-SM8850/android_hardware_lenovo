// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.ActivityTaskManager
import android.app.TaskStackListener
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.os.Handler
import android.os.UserHandle
import android.util.Log

// All state and callbacks are confined to the ring worker.
class ReactiveSession(
    private val context: Context,
    private val handler: Handler,
    private val changed: () -> Unit,
) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private var tracking = false
    private var taskRegistered = false
    private var playback = emptyList<AudioPlaybackConfiguration>()
    private var gameUid: Int? = null
    private val retry = Runnable {
        failed = false
        changed()
    }
    private val queryTask = Runnable {
        if (!tracking) return@Runnable
        try {
            val task = ActivityTaskManager.getInstance().getTasks(1).firstOrNull()
            val packageName = task?.topActivity?.packageName
            val app = if (task?.userId == UserHandle.USER_SYSTEM && packageName != null) {
                context.packageManager.getApplicationInfo(packageName, 0)
            } else null
            gameUid = app?.takeIf { it.category == ApplicationInfo.CATEGORY_GAME }?.uid
            changed()
        } catch (_: PackageManager.NameNotFoundException) {
            gameUid = null
            changed()
        } catch (exception: RuntimeException) {
            fail(exception)
        }
    }
    private val tasks = object : TaskStackListener() {
        override fun onTaskStackChanged() {
            handler.removeCallbacks(queryTask)
            handler.post(queryTask)
        }
    }
    private val callback = object : AudioManager.AudioPlaybackCallback() {
        override fun onPlaybackConfigChanged(configs: List<AudioPlaybackConfiguration>) {
            if (!tracking) return
            playback = configs
            changed()
        }
    }
    var failed = false
        private set

    fun track(enabled: Boolean) {
        if (!enabled) {
            stop()
            failed = false
            return
        }
        if (tracking || failed) return
        try {
            tracking = true
            audio.registerAudioPlaybackCallback(callback, handler)
            ActivityTaskManager.getService().registerTaskStackListener(tasks)
            taskRegistered = true
            playback = audio.activePlaybackConfigurations
            queryTask.run()
        } catch (exception: Exception) {
            fail(exception)
        }
    }

    private fun fail(exception: Exception) {
        Log.w("HaloReactive", "Session tracking stopped", exception)
        suspend()
        handler.postDelayed(retry, 2000)
        changed()
    }

    fun pauseForHal() {
        stop()
    }

    fun suspend() {
        stop()
        failed = true
    }

    private fun stop() {
        if (tracking) audio.unregisterAudioPlaybackCallback(callback)
        if (taskRegistered) {
            try {
                ActivityTaskManager.getService().unregisterTaskStackListener(tasks)
            } catch (exception: Exception) {
                Log.w("HaloReactive", "Cannot remove task listener", exception)
            }
        }
        tracking = false
        taskRegistered = false
        playback = emptyList()
        gameUid = null
        handler.removeCallbacks(queryTask)
        handler.removeCallbacks(retry)
    }

    fun scene(music: Boolean, games: Boolean): String {
        if (!tracking || failed) return "idle"
        if (games && gameUid != null) return "game"
        if (music && playback.any { isOwnerPlayback(it) &&
                it.audioAttributes.usage == AudioAttributes.USAGE_MEDIA }) return "media"
        return "idle"
    }

    fun hasAudio(music: Boolean, games: Boolean): Boolean = playback.any {
        val usage = it.audioAttributes.usage
        isOwnerPlayback(it) && ((music && usage == AudioAttributes.USAGE_MEDIA) ||
            (games && it.clientUid == gameUid &&
                (usage == AudioAttributes.USAGE_MEDIA || usage == AudioAttributes.USAGE_GAME)))
    }

    private fun isOwnerPlayback(config: AudioPlaybackConfiguration): Boolean =
        // Muted players are not "active", but still represent an ongoing playback session.
        config.playerState == AudioPlaybackConfiguration.PLAYER_STATE_STARTED && config.clientUid >= 0 &&
            UserHandle.getUserId(config.clientUid) == UserHandle.USER_SYSTEM
}
