// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.UserHandle;
import android.provider.Settings;
import android.service.notification.StatusBarNotification;
import android.telephony.SubscriptionManager;

import java.util.HashMap;
import java.util.Map;

/** Owner-only, durable suggestions for carriers without a built-in modem profile. */
final class UnsupportedCarrierNotice {
    static final String CHANNEL = "unsupported_carrier";
    private static final String OBSERVED = "notice_observed_";
    private static final String NOTIFIED = "notice_notified_";
    private final Context context;
    private final SharedPreferences prefs;
    private final NotificationManager manager;
    private Map<Integer, String> current = new HashMap<>();

    UnsupportedCarrierNotice(Context context, SharedPreferences prefs) {
        this.context = context;
        this.prefs = prefs;
        manager = context.getSystemService(NotificationManager.class);
        NotificationChannel channel = new NotificationChannel(CHANNEL,
                context.getString(R.string.unsupported_carrier_channel_name),
                NotificationManager.IMPORTANCE_DEFAULT);
        channel.setDescription(context.getString(R.string.unsupported_carrier_channel_description));
        manager.createNotificationChannel(channel);
    }

    void observe(Map<Integer, String> identities) {
        boolean automatic = prefs.getBoolean("automatic", false);
        for (int subId : current.keySet())
            if (!identities.containsKey(subId)) manager.cancel(CHANNEL, subId);
        SharedPreferences.Editor edit = prefs.edit();
        for (Map.Entry<Integer, String> entry : identities.entrySet()) {
            int subId = entry.getKey();
            String identity = entry.getValue();
            if (CoordinatorState.shouldResetCarrierNotice(automatic, identity,
                    prefs.getString(OBSERVED + subId, ""))) {
                edit.putString(OBSERVED + subId, identity).remove(NOTIFIED + subId);
                manager.cancel(CHANNEL, subId);
            }
        }
        edit.apply();
        current = new HashMap<>(identities);
        show(identities);
    }

    void cancel() {
        for (StatusBarNotification notice : manager.getActiveNotifications())
            if (CHANNEL.equals(notice.getTag())) manager.cancel(CHANNEL, notice.getId());
    }

    void clearIfAutomatic() {
        if (!prefs.getBoolean("automatic", false)) return;
        SharedPreferences.Editor edit = prefs.edit();
        for (String key : prefs.getAll().keySet())
            if (key.startsWith(NOTIFIED)) edit.remove(key);
        edit.apply();
        cancel();
    }

    void show(Map<Integer, String> identities) {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM || !current.equals(identities))
            return;
        if (prefs.getBoolean("automatic", false)) {
            cancel();
            return;
        }
        for (Map.Entry<Integer, String> entry : identities.entrySet())
            if (!CoordinatorState.shouldNotifyUnsupported(false, entry.getValue(), ""))
                manager.cancel(CHANNEL, entry.getKey());
        if (Settings.Global.getInt(context.getContentResolver(),
                        Settings.Global.AIRPLANE_MODE_ON, 0) != 0
                || !manager.areNotificationsEnabled()
                || manager.getNotificationChannel(CHANNEL).getImportance()
                        == NotificationManager.IMPORTANCE_NONE)
            return;
        for (Map.Entry<Integer, String> entry : identities.entrySet()) {
            int subId = entry.getKey();
            String identity = entry.getValue();
            if (!CoordinatorState.shouldNotifyUnsupported(prefs.getBoolean("automatic", false),
                    identity, prefs.getString(NOTIFIED + subId, "")))
                continue;
            Intent intent = new Intent(context, McfgStatusActivity.class)
                    .setAction("io.github.miner7222.mcfg.CARRIER_NOTICE." + subId)
                    .putExtra(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            PendingIntent content = PendingIntent.getActivity(context, subId, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            String text = context.getString(R.string.unsupported_carrier_text);
            Notification notification = new Notification.Builder(context, CHANNEL)
                    .setSmallIcon(R.drawable.ic_carrier_warning)
                    .setContentTitle(context.getString(R.string.unsupported_carrier_title))
                    .setContentText(text)
                    .setStyle(new Notification.BigTextStyle().bigText(text))
                    .setContentIntent(content)
                    .setAutoCancel(true)
                    .build();
            // Persist before posting so a process restart cannot repeat the alert.
            if (prefs.edit().putString(NOTIFIED + subId, identity).commit())
                manager.notify(CHANNEL, subId, notification);
        }
    }
}
