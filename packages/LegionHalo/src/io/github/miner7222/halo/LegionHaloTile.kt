// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.SharedPreferences
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

class LegionHaloTile : TileService() {
    private val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ -> update() }
    override fun onStartListening() {
        super.onStartListening()
        RingController.preferences.registerOnSharedPreferenceChangeListener(listener)
        update()
    }
    override fun onStopListening() {
        RingController.preferences.unregisterOnSharedPreferenceChangeListener(listener)
        super.onStopListening()
    }
    override fun onClick() {
        super.onClick()
        RingController.preferences.edit().putBoolean("enabled", !RingController.enabled()).apply()
        update()
    }
    private fun update() {
        val tile = qsTile ?: return
        tile.state = if (RingController.enabled()) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        tile.subtitle = if (RingController.enabled() && !RingController.allowed()) getString(R.string.paused)
            else if (!RingController.connected) getString(R.string.service_unavailable) else null
        tile.updateTile()
    }
}
