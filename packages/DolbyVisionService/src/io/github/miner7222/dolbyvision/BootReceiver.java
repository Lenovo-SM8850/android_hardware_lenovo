/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** Starts DolbyVisionService on locked and unlocked boot. */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = DolbyVisionService.TAG;

    @Override
    public void onReceive(Context context, Intent intent) {
        final String action = intent.getAction();
        if (!Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action)
                && !Intent.ACTION_BOOT_COMPLETED.equals(action)) {
            return;
        }
        Log.i(TAG, "Received " + action + ", starting service");
        try {
            context.startService(new Intent(context, DolbyVisionService.class));
        } catch (IllegalStateException | SecurityException e) {
            Log.e(TAG, "Failed to start DolbyVisionService", e);
        }
    }
}
