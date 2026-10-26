// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.SystemClock

class ContentColors(private val context: Context, private val handler: Handler,
        private val changed: () -> Unit) {
    private val media = MediaArtColors(context, handler, changed)
    private val game = ContentColorSmoother()
    private var scene = "idle"
    private var gamePackage: String? = null
    private var lastDemand: Pair<Boolean, String?>? = null
    private var fallback = 0xff0000
    private val animate = object : Runnable {
        override fun run() {
            changed()
            if (scene == "game" && game.moving()) handler.postDelayed(this, 125)
        }
    }
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == ACTION_QUERY) {
                sendDemand(force = true)
            } else if (scene == "game" && gamePackage != null &&
                    intent.getStringExtra("package") == gamePackage) {
                val color = intent.getIntExtra("color", -1).takeIf { ContentColorSmoother.usable(it) }
                game.setTarget(color ?: fallback, SystemClock.uptimeMillis())
                handler.removeCallbacks(animate)
                handler.post(animate)
            }
        }
    }

    init {
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(ACTION_QUERY)
            addAction(ACTION_COLOR)
        }, Manifest.permission.WRITE_SECURE_SETTINGS, handler, Context.RECEIVER_EXPORTED)
    }

    fun track(nextScene: String, nextPackage: String?, music: Boolean) {
        media.track(music)
        if (scene != nextScene || gamePackage != nextPackage) {
            scene = nextScene
            gamePackage = nextPackage
            game.clear()
            handler.removeCallbacks(animate)
        }
        sendDemand()
    }

    fun color(selected: Int): Int {
        fallback = selected
        return when (scene) {
            "game" -> game.value(selected, SystemClock.uptimeMillis())
            "media" -> media.color ?: selected
            else -> selected
        }
    }

    private fun sendDemand(force: Boolean = false) {
        val demand = (scene == "game" && gamePackage != null) to gamePackage
        if (!force && lastDemand == demand) return
        lastDemand = demand
        context.sendBroadcast(Intent(ACTION_DEMAND).setPackage("io.chaldeaprjkt.gamespace")
                .putExtra("enabled", demand.first).putExtra("package", demand.second),
                Manifest.permission.WRITE_SECURE_SETTINGS)
    }

    companion object {
        private const val ACTION_DEMAND = "io.github.miner7222.halo.GAME_COLOR_DEMAND"
        private const val ACTION_QUERY = "io.github.miner7222.halo.QUERY_GAME_COLOR_DEMAND"
        private const val ACTION_COLOR = "io.github.miner7222.halo.GAME_COLOR"
    }
}
