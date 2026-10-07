// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (android.os.UserHandle.myUserId() != android.os.UserHandle.USER_SYSTEM) return;
        // These actions are protected framework broadcasts. Ignore other actions
        // even if a caller holds the receiver's permission.
        String action = intent.getAction();
        if (Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_BOOT_COMPLETED.equals(action)
                || "android.telephony.action.SIM_APPLICATION_STATE_CHANGED".equals(action)
                || "android.telephony.action.SIM_CARD_STATE_CHANGED".equals(action)) {
            context.startService(new Intent(context, McfgCoordinatorService.class));
        }
    }
}
