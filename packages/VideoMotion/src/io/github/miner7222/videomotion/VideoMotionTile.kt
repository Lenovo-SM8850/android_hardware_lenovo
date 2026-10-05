// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.videomotion

import android.content.SharedPreferences
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

class VideoMotionTile : TileService() {
    private val listener =
        SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == MotionController.ENABLED) update()
        }

    override fun onStartListening() {
        super.onStartListening()
        MotionController.preferences.registerOnSharedPreferenceChangeListener(listener)
        update()
    }

    override fun onStopListening() {
        MotionController.preferences.unregisterOnSharedPreferenceChangeListener(listener)
        super.onStopListening()
    }

    override fun onClick() {
        super.onClick()
        if (MotionController.isOwner()) {
            unlockAndRun {
                MotionController.setEnabled(!MotionController.enabled())
                update()
            }
        }
    }

    private fun update() {
        val tile = qsTile ?: return
        tile.state =
            when {
                !MotionController.isOwner() -> Tile.STATE_UNAVAILABLE
                MotionController.enabled() -> Tile.STATE_ACTIVE
                else -> Tile.STATE_INACTIVE
            }
        tile.subtitle = if (MotionController.isOwner()) null else getString(R.string.user_limit)
        tile.updateTile()
    }
}
