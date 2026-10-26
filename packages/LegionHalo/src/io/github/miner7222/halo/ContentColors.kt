// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

import android.content.Context
import android.os.Handler

class ContentColors(context: Context, handler: Handler, changed: () -> Unit) {
    private val media = MediaArtColors(context, handler, changed)
    private var scene = "idle"

    fun track(nextScene: String, music: Boolean) {
        media.track(music)
        scene = nextScene
    }

    fun color(selected: Int): Int {
        return when (scene) {
            "media" -> media.color ?: selected
            else -> selected
        }
    }
}
