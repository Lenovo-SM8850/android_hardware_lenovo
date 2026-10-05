/*
 * SPDX-License-Identifier: Apache-2.0
 */
package io.github.miner7222.pen

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class BootCompletedReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) {
            context.startService(Intent(context, PenService::class.java))
        }
    }
}
