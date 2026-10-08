/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.datasim;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(intent.getAction())
                || Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            ((DataSimPolicyApplication) context.getApplicationContext()).onBoot();
        }
    }
}
