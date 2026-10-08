/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.datasim;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.provider.Settings;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import android.telephony.UiccSlotInfo;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/** Applies the built-in data SIM default during the first boot session only. */
public final class DataSimPolicyApplication extends Application {
    private static final String TAG = "DataSimPolicy";
    private static final String SLOT_PROPERTY = "ro.lenovo.datasim.slot";
    private static final String DONE = "done";

    // Accessed only on the main thread, including subscription callbacks.
    private SharedPreferences mPreferences;
    private SubscriptionManager mSubscriptions;
    private TelephonyManager mTelephony;
    private int mPhysicalSlot;
    private boolean mStarted;
    private boolean mListening;
    private final SubscriptionManager.OnSubscriptionsChangedListener mListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    applyDefault();
                }
            };
    private final BroadcastReceiver mShutdownReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    finishFirstBoot();
                }
            };

    void onBoot() {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM || mStarted) {
            return;
        }
        mStarted = true;
        mPhysicalSlot = SystemProperties.getInt(SLOT_PROPERTY, -1);
        if (mPhysicalSlot < 0) {
            return;
        }
        mPreferences =
                createDeviceProtectedStorageContext()
                        .getSharedPreferences("first_boot", MODE_PRIVATE);
        String done = mPreferences.getString(DONE, null);
        if (FirstBootPolicy.COMPLETE.equals(done)) {
            return;
        }
        int bootCount =
                Settings.Global.getInt(getContentResolver(), Settings.Global.BOOT_COUNT, -1);
        if (!FirstBootPolicy.mayHandle(done, bootCount)) {
            // The first session ended without a card, including an unclean shutdown.
            finishFirstBoot();
            return;
        }
        // Persist session admission before waiting, without retaining any card identifiers.
        if (!mPreferences.edit().putString(DONE, FirstBootPolicy.pending(bootCount)).commit()) {
            Log.e(TAG, "Cannot record first boot session; leaving SIM unchanged");
            return;
        }
        mSubscriptions = getSystemService(SubscriptionManager.class);
        mTelephony = getSystemService(TelephonyManager.class);
        if (mSubscriptions == null || mTelephony == null) {
            finishFirstBoot();
            return;
        }
        registerReceiver(
                mShutdownReceiver,
                new IntentFilter(Intent.ACTION_SHUTDOWN),
                Context.RECEIVER_NOT_EXPORTED);
        mListening = true;
        mSubscriptions.addOnSubscriptionsChangedListener(getMainExecutor(), mListener);
        applyDefault();
    }

    private void applyDefault() {
        if (!mListening) {
            return;
        }
        try {
            UiccSlotInfo[] slots = mTelephony.getUiccSlotsInfo();
            if (slots == null || mPhysicalSlot >= slots.length) {
                return;
            }
            UiccSlotInfo slot = slots[mPhysicalSlot];
            if (slot == null
                    || slot.getCardStateInfo() != UiccSlotInfo.CARD_STATE_INFO_PRESENT
                    || slot.getIsEuicc()) {
                return;
            }
            // This array is indexed by physical slot. For a fixed UICC, cardId is the ICCID.
            // Match that identity, including disabled records whose logical slot is -1.
            List<FirstBootPolicy.Card> cards =
                    List.of(new FirstBootPolicy.Card(mPhysicalSlot, true, slot.getCardId()));
            List<FirstBootPolicy.Subscription> subscriptions = new ArrayList<>();
            for (SubscriptionInfo subscription :
                    mSubscriptions.getAvailableSubscriptionInfoList()) {
                subscriptions.add(
                        new FirstBootPolicy.Subscription(
                                subscription.getSubscriptionId(), subscription.getIccId()));
            }
            int subId = FirstBootPolicy.selectSubscription(mPhysicalSlot, cards, subscriptions);
            if (subId == FirstBootPolicy.NO_SUBSCRIPTION) {
                return;
            }
            // Stop before the call: changing the record itself triggers a subscription callback.
            stopListening();
            mSubscriptions.setUiccApplicationsEnabled(subId, false);
            finishFirstBoot();
            Log.i(TAG, "Applied first boot data SIM default");
        } catch (IllegalArgumentException | SecurityException | UnsupportedOperationException e) {
            // Do not turn a failed first-boot attempt into an override on a subsequent boot.
            finishFirstBoot();
            Log.e(TAG, "Cannot apply first boot data SIM default", e);
        }
    }

    private void finishFirstBoot() {
        stopListening();
        if (!mPreferences.edit().putString(DONE, FirstBootPolicy.COMPLETE).commit()) {
            Log.e(TAG, "Cannot record first boot completion");
        }
    }

    private void stopListening() {
        if (mListening) {
            mListening = false;
            mSubscriptions.removeOnSubscriptionsChangedListener(mListener);
            unregisterReceiver(mShutdownReceiver);
        }
    }
}
