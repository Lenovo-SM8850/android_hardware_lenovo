// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.content.Intent;
import android.os.Bundle;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.view.View;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.google.android.material.appbar.AppBarLayout;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class McfgStatusActivity extends CollapsingToolbarBaseActivity {
    private static final String MAIN = "main";
    private static final int MAX_CONTENT_DP = 840;
    private final Map<Integer, CallCallback> callbacks = new HashMap<>();
    private boolean resumed;
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    syncCallbacks();
                }
            };

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
            getSupportFragmentManager().beginTransaction().add(frame, fragment, MAIN).commit();
        }
        getTheme().applyStyle(R.style.ThemeOverlay_McfgLoader, true);
        constrainWidth();
        // Start with the large title in every orientation.
        AppBarLayout appBar = getAppBarLayout();
        if (appBar != null) appBar.post(() -> appBar.setExpanded(true, false));
        if (android.os.UserHandle.myUserId() == android.os.UserHandle.USER_SYSTEM)
            startService(new Intent(this, McfgCoordinatorService.class));
    }

    // Single-pane content stays in a centered column on wide windows.
    private void constrainWidth() {
        View root = findViewById(com.android.settingslib.collapsingtoolbar.R.id.content_parent);
        if (root == null) return;
        root.addOnLayoutChangeListener(
                (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                    float density = view.getResources().getDisplayMetrics().density;
                    int max = Math.round(MAX_CONTENT_DP * density);
                    int side = Math.max(0, (right - left - max) / 2);
                    if (view.getPaddingLeft() != side || view.getPaddingRight() != side)
                        view.setPadding(side, view.getPaddingTop(), side, view.getPaddingBottom());
                });
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        getSystemService(SubscriptionManager.class)
                .addOnSubscriptionsChangedListener(getMainExecutor(), subListener);
        syncCallbacks();
    }

    @Override
    protected void onPause() {
        resumed = false;
        getSystemService(SubscriptionManager.class)
                .removeOnSubscriptionsChangedListener(subListener);
        for (CallCallback cb : callbacks.values()) cb.manager.unregisterTelephonyCallback(cb);
        callbacks.clear();
        super.onPause();
    }

    private void syncCallbacks() {
        if (!resumed) return;
        List<SubscriptionInfo> active = CarrierSettingsFragment.presentSubscriptions(this);
        List<Integer> ids = new ArrayList<>();
        if (active != null)
            for (SubscriptionInfo info : active) ids.add(info.getSubscriptionId());
        if (ids.isEmpty()) ids.add(SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        for (int id : new ArrayList<>(callbacks.keySet()))
            if (!ids.contains(id)) {
                CallCallback cb = callbacks.remove(id);
                cb.manager.unregisterTelephonyCallback(cb);
            }
        for (int id : ids)
            if (!callbacks.containsKey(id)) {
                TelephonyManager manager = getSystemService(TelephonyManager.class);
                if (id != SubscriptionManager.INVALID_SUBSCRIPTION_ID)
                    manager = manager.createForSubscriptionId(id);
                CallCallback cb = new CallCallback(manager);
                callbacks.put(id, cb);
                manager.registerTelephonyCallback(getMainExecutor(), cb);
            }
        updateSwitches();
    }

    private void updateSwitches() {
        boolean idle = !callbacks.isEmpty();
        for (CallCallback cb : callbacks.values())
            if (cb.callState != TelephonyManager.CALL_STATE_IDLE) idle = false;
        // The main screen may sit on the back stack below the overrides screen.
        CarrierSettingsFragment fragment =
                (CarrierSettingsFragment) getSupportFragmentManager().findFragmentByTag(MAIN);
        if (fragment != null) fragment.setSwitchesEnabled(idle);
    }

    private final class CallCallback extends TelephonyCallback
            implements TelephonyCallback.CallStateListener {
        final TelephonyManager manager;
        int callState = -1;

        CallCallback(TelephonyManager manager) {
            this.manager = manager;
        }

        @Override
        public void onCallStateChanged(int state) {
            if (!callbacks.containsValue(this)) return;
            callState = state;
            updateSwitches();
        }
    }
}
