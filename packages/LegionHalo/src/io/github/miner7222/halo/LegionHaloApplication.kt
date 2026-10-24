// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.halo

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class LegionHaloApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        RingController.initialize(this)
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        RingController.initialize(context.applicationContext)
        RingController.refresh()
    }
}
