// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.util.Log
import androidx.palette.graphics.Palette
import kotlin.math.max

/** Metadata callbacks and bounded palette work run on the ring worker, never the UI thread. */
class MediaArtColors(context: Context, private val handler: Handler, private val changed: () -> Unit) {
    private val sessions = context.getSystemService(MediaSessionManager::class.java)
    private var tracking = false
    private val controllers = mutableListOf<MediaController>()
    private var cacheKey: Pair<String, Int>? = null
    private var cachedColor: Int? = null
    var color: Int? = null
        private set
    private val callback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = update()
        override fun onPlaybackStateChanged(state: PlaybackState?) = update()
        override fun onSessionDestroyed() = update()
    }
    private val listener = MediaSessionManager.OnActiveSessionsChangedListener {
        bind(it ?: emptyList())
    }

    fun track(enabled: Boolean) {
        if (enabled == tracking) return
        tracking = enabled
        if (!enabled) {
            sessions.removeOnActiveSessionsChangedListener(listener)
            controllers.forEach { it.unregisterCallback(callback) }
            controllers.clear()
            publish(null)
            return
        }
        try {
            sessions.addOnActiveSessionsChangedListener(listener, null, handler)
            bind(sessions.getActiveSessions(null))
        } catch (exception: RuntimeException) {
            Log.w("HaloContentColor", "Media artwork unavailable", exception)
            track(false)
        }
    }

    private fun bind(next: List<MediaController>) {
        if (!tracking) return
        controllers.forEach { it.unregisterCallback(callback) }
        controllers.clear()
        controllers.addAll(next)
        controllers.forEach { it.registerCallback(callback, handler) }
        update()
    }

    private fun update() {
        if (!tracking) return
        try {
            val controller = controllers.firstOrNull {
                it.playbackState?.state == PlaybackState.STATE_PLAYING
            } ?: run { publish(null); return }
            val metadata = controller.metadata
            val artwork = metadata?.getBitmap(MediaMetadata.METADATA_KEY_ART)
                    ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
                    ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            if (metadata == null || artwork == null || artwork.isRecycled) {
                publish(null)
                return
            }
            val scale = minOf(1.0, 64.0 / max(artwork.width, artwork.height))
            val scaled = Bitmap.createScaledBitmap(artwork,
                    max(1, (artwork.width * scale).toInt()), max(1, (artwork.height * scale).toInt()), true)
            val bitmap = try { scaled.copy(Bitmap.Config.ARGB_8888, false) }
                    finally { if (scaled !== artwork) scaled.recycle() }
            if (bitmap == null) { publish(null); return }
            try {
                val pixels = IntArray(bitmap.width * bitmap.height)
                bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
                val track = listOf(MediaMetadata.METADATA_KEY_MEDIA_ID, MediaMetadata.METADATA_KEY_TITLE,
                        MediaMetadata.METADATA_KEY_ARTIST, MediaMetadata.METADATA_KEY_ALBUM)
                        .joinToString("|") { metadata.getString(it).orEmpty() }
                val key = (controller.packageName + "|" + track) to pixels.contentHashCode()
                if (cacheKey != key) {
                    val palette = Palette.from(bitmap).maximumColorCount(16).clearFilters()
                            .addFilter { _, hsl -> hsl[2] in 0.15f..0.90f }.generate()
                    cachedColor = (palette.vibrantSwatch ?: palette.dominantSwatch)?.rgb?.and(0xffffff)
                    cacheKey = key
                    Log.i("HaloContentColor", "Artwork palette colour=${cachedColor?.let { "%06x".format(it) } ?: "fallback"}")
                }
                publish(cachedColor)
            } finally { bitmap.recycle() }
        } catch (exception: RuntimeException) {
            Log.w("HaloContentColor", "Media artwork unavailable", exception)
            publish(null)
        }
    }

    private fun publish(next: Int?) {
        if (color == next) return
        color = next
        changed()
    }
}
