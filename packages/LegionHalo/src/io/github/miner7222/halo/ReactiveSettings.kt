// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.content.Context
import android.os.UserHandle
import android.provider.Settings

object ReactiveSettings {
    // Shared with the in-game panel. Only holders of WRITE_SECURE_SETTINGS can write it.
    const val ENABLED = "halo_reactive_enabled"
    const val AVAILABLE = "halo_ring_available"

    fun enabled(context: Context): Boolean = Settings.Secure.getIntForUser(
        context.contentResolver, ENABLED, 0, UserHandle.USER_SYSTEM
    ) == 1

    fun setEnabled(context: Context, enabled: Boolean): Boolean =
        UserHandle.myUserId() == UserHandle.USER_SYSTEM && Settings.Secure.putIntForUser(
            context.contentResolver, ENABLED, if (enabled) 1 else 0, UserHandle.USER_SYSTEM
        )
}
