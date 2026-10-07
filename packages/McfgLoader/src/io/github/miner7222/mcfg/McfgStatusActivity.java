// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.content.Intent;
import android.os.Bundle;
import android.telephony.SubscriptionManager;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;

public final class McfgStatusActivity extends CollapsingToolbarBaseActivity {
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        int frame = com.android.settingslib.collapsingtoolbar.R.id.content_frame;
        if (getSupportFragmentManager().findFragmentById(frame) == null) {
            CarrierSettingsFragment fragment = new CarrierSettingsFragment();
            Bundle args = new Bundle();
            args.putInt(
                    SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX,
                    getIntent()
                            .getIntExtra(
                                    SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX,
                                    SubscriptionManager.INVALID_SUBSCRIPTION_ID));
            fragment.setArguments(args);
            getSupportFragmentManager().beginTransaction().add(frame, fragment).commit();
        }
        if (android.os.UserHandle.myUserId() == android.os.UserHandle.USER_SYSTEM)
            startService(new Intent(this, McfgCoordinatorService.class));
    }
}
