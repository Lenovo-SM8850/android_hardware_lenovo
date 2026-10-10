// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemProperties;
import android.os.SystemClock;
import android.telephony.CarrierConfigManager;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;

import vendor.lenovo.hardware.mcfg.IMcfgLoader;
import vendor.lenovo.hardware.mcfg.McfgStatus;

import java.util.ArrayList;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.atomic.AtomicBoolean;

public final class McfgCoordinatorService extends Service {
    static final String NAME = "vendor.lenovo.hardware.mcfg.IMcfgLoader/default";
    static final String PROBE = "io.github.miner7222.mcfg.PROBE";
    static final String RESTORE = "io.github.miner7222.mcfg.RESTORE";
    static final String ENABLE = "io.github.miner7222.mcfg.ENABLE";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService transactions = Executors.newSingleThreadExecutor();
    private final ExecutorService contextIpc = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean();
    private final Map<Integer, SlotCallback> callbacks = new HashMap<>();
    private volatile IMcfgLoader worker;
    private volatile String[] home = new String[0];
    private volatile long contextEpoch;
    private long verifiedEpoch = -1, verifiedConfigVersion = -1;
    private long restoredEpoch = -1;
    private volatile boolean contextSafe;
    private SharedPreferences prefs;
    private String bootId = "";
    private SubscriptionManager subscriptions;
    private final CoordinatorState state = new CoordinatorState();
    private volatile long configVersion;
    private long configuredVersion = -1;
    private boolean automaticEnabled;
    private volatile boolean restoreRequested;
    private volatile McfgStatus lastStatus;
    private boolean tickReady;
    private final SharedPreferences.OnSharedPreferenceChangeListener preferenceListener =
            (preferences, key) -> {
                if ("automatic".equals(key) || "disable_requested".equals(key)
                        || "config_version".equals(key)) wakeHeartbeat();
            };
    private final BroadcastReceiver carrierReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            wakeHeartbeat();
        }
    };
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    tick();
                }
            };
    private final Runnable heartbeat =
            new Runnable() {
                @Override
                public void run() {
                    tick();
                }
            };

    @Override
    public void onCreate() {
        super.onCreate();
        prefs = createDeviceProtectedStorageContext().getSharedPreferences("mcfg", MODE_PRIVATE);
        configVersion = prefs.getLong("config_version", 0);
        verifiedConfigVersion = prefs.getLong("verified_config_version", 0);
        bootId = readBootId();
        automaticEnabled = prefs.getBoolean("automatic", true);
        subscriptions = getSystemService(SubscriptionManager.class);
        subscriptions.addOnSubscriptionsChangedListener(getMainExecutor(), subListener);
        NotificationManager manager = getSystemService(NotificationManager.class);
        // Remove the status notification and channel left by earlier versions.
        manager.cancel(1);
        manager.deleteNotificationChannel("mcfg");
        prefs.registerOnSharedPreferenceChangeListener(preferenceListener);
        IntentFilter filter = new IntentFilter(CarrierConfigManager.ACTION_CARRIER_CONFIG_CHANGED);
        filter.addAction(TelephonyManager.ACTION_SIM_APPLICATION_STATE_CHANGED);
        filter.addAction(TelephonyManager.ACTION_SIM_CARD_STATE_CHANGED);
        filter.addAction(Intent.ACTION_AIRPLANE_MODE_CHANGED);
        registerReceiver(carrierReceiver, filter, Context.RECEIVER_EXPORTED);
        wakeHeartbeat();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && PROBE.equals(intent.getAction())) {
            transactions.execute(
                    () -> {
                        try {
                            connect();
                            renewContext(contextEpoch);
                            publish(worker.probeReadOnly());
                        } catch (Exception e) {
                            report("Read-only probe failed: " + e.getClass().getSimpleName());
                        }
                    });
        } else if (intent != null && RESTORE.equals(intent.getAction())) {
            prefs.edit()
                    .putBoolean("automatic", false)
                    .putBoolean("disable_requested", true)
                    .commit();
            restoreRequested = true;
        }
        boolean enabled = prefs.getBoolean("automatic", true);
        if (CoordinatorState.shouldConfigureAutomatic(enabled, automaticEnabled,
                intent != null && (ENABLE.equals(intent.getAction())
                        || RESTORE.equals(intent.getAction())))) {
            automaticEnabled = enabled;
            requestConfiguration(prefs);
        }
        configVersion = prefs.getLong("config_version", 0);
        tick();
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        contextSafe = false;
        sendContext();
        main.removeCallbacks(heartbeat);
        prefs.unregisterOnSharedPreferenceChangeListener(preferenceListener);
        unregisterReceiver(carrierReceiver);
        subscriptions.removeOnSubscriptionsChangedListener(subListener);
        for (SlotCallback cb : callbacks.values()) cb.manager.unregisterTelephonyCallback(cb);
        transactions.shutdown();
        contextIpc.shutdown();
        super.onDestroy();
    }

    private static IBinder binder(String name) throws Exception {
        return (IBinder)
                Class.forName("android.os.ServiceManager")
                        .getMethod("checkService", String.class)
                        .invoke(null, name);
    }

    private synchronized void connect() throws Exception {
        if (worker != null && worker.asBinder().isBinderAlive()) return;
        IBinder binder = binder(NAME);
        if (binder == null) throw new IllegalStateException("loader unavailable");
        worker = IMcfgLoader.Stub.asInterface(binder);
        binder.linkToDeath(
                () -> {
                    if (worker != null && worker.asBinder() == binder) worker = null;
                    contextSafe = false;
                    main.post(() -> {
                        lastStatus = null;
                        wakeHeartbeat();
                    });
                },
                0);
    }

    private void syncCallbacks(List<SubscriptionInfo> active) {
        List<Integer> ids = new ArrayList<>();
        for (SubscriptionInfo info : active) ids.add(info.getSubscriptionId());
        // The default phone still exists without a SIM and witnesses its SSR.
        if (ids.isEmpty()) ids.add(SubscriptionManager.INVALID_SUBSCRIPTION_ID);
        for (int id : new ArrayList<>(callbacks.keySet()))
            if (!ids.contains(id)) {
                SlotCallback cb = callbacks.remove(id);
                cb.manager.unregisterTelephonyCallback(cb);
            }
        for (int id : ids)
            if (!callbacks.containsKey(id)) {
                TelephonyManager manager = getSystemService(TelephonyManager.class);
                SlotCallback cb = new SlotCallback(id == SubscriptionManager.INVALID_SUBSCRIPTION_ID
                        ? manager : manager.createForSubscriptionId(id));
                cb.manager.registerTelephonyCallback(getMainExecutor(), cb);
                callbacks.put(id, cb);
            }
    }

    private void wakeHeartbeat() {
        if (transactions.isShutdown()) return;
        tickReady = false;
        if (!main.hasCallbacks(heartbeat)) main.post(heartbeat);
    }

    private void updateHeartbeat() {
        if (transactions.isShutdown()) return;
        McfgStatus status = lastStatus;
        boolean enabled = prefs.getBoolean("automatic", false);
        boolean pending = status != null && (CoordinatorState.isRestartPending(status.state)
                || CoordinatorState.shouldRecover(status.state, true, true));
        boolean needed = busy.get() || (enabled && (!tickReady || home.length > 0))
                || restoreRequested || prefs.getBoolean("disable_requested", false)
                || !prefs.getString("restart_token", "").isEmpty() || pending
                || (tickReady && CoordinatorState.shouldCoordinate(enabled, home.length)
                        && (status == null || configuredVersion != configVersion));
        if (!needed) main.removeCallbacks(heartbeat);
        else if (!main.hasCallbacks(heartbeat)) main.postDelayed(heartbeat, 1000);
    }

    private void tick() {
        tickReady = false;
        configVersion = prefs.getLong("config_version", 0);
        try {
            if (!prefs.getString("restart_token", "").isEmpty()
                    && CoordinatorState.rebooted(prefs.getString("restart_boot_id", ""), bootId))
                prefs.edit().putBoolean("restart_saw_down", true).commit();
            List<SubscriptionInfo> active = CarrierSettingsFragment.presentSubscriptions(this);
            if (active == null || active.size() > 2)
                throw new IllegalStateException("identity unavailable");
            active.sort(Comparator.comparingInt(SubscriptionInfo::getSimSlotIndex));
            syncCallbacks(active);
            boolean radiosReady = true;
            for (SlotCallback cb : callbacks.values()) radiosReady &= cb.radioOn;
            state.setRadiosReady(radiosReady);
            if (!radiosReady) throw new IllegalStateException("radio not ready");
            List<String> operators = new ArrayList<>();
            for (SubscriptionInfo info : active) {
                // A privileged carrier app is layered after vendor.xml by
                // CarrierConfigLoader. Pin only the entry keys when it replaces them.
                CarrierOverrides entry = new CarrierOverrides(this, info.getSubscriptionId());
                android.os.PersistableBundle carrier = entry.config();
                if (!carrier.getBoolean(
                                android.telephony.CarrierConfigManager
                                        .KEY_CARRIER_SETTINGS_ENABLE_BOOL)
                        || !CarrierOverrides.COMPONENT.equals(
                                carrier.getString(
                                        android.telephony.CarrierConfigManager
                                                .KEY_CARRIER_SETTINGS_ACTIVITY_COMPONENT_NAME_STRING)))
                    entry.publish(new android.os.PersistableBundle());
                TelephonyManager manager = callbacks.get(info.getSubscriptionId()).manager;
                if (manager.getSimApplicationState() != TelephonyManager.SIM_STATE_LOADED)
                    throw new IllegalStateException("SIM not loaded");
                String operator = manager.getSimOperator(); // Home identity, never serving PLMN.
                if (!operator.matches("[0-9]{5,6}"))
                    throw new IllegalStateException("home operator unknown");
                operators.add(operator);
            }
            operators.sort(String::compareTo);
            String nextIdentity = active.isEmpty() ? "no_sim" : String.join(";", operators);
            state.observe(nextIdentity);
            home = operators.toArray(new String[0]);
            contextEpoch = state.epoch();
            // Revoke automatic leases immediately on removal, including queued
            // control-only completions/claims; explicit OFF can still use the radio.
            contextSafe = CoordinatorState.shouldCoordinate(prefs.getBoolean("automatic", true), home.length);
            sendContext();
            if (!busy.getAndSet(true)) transactions.execute(this::coordinate);
            tickReady = true;
        } catch (Exception e) {
            home = new String[0];
            state.clear();
            contextSafe = false;
            sendContext();
            report("Waiting for loaded SIMs");
        } finally {
            updateHeartbeat();
        }
    }

    private void sendContext() {
        final String[] snapshot = home.clone();
        final boolean safe = contextSafe;
        final long epoch = contextEpoch;
        final String baseband = SystemProperties.get("gsm.version.baseband", "");
        contextIpc.execute(
                () -> {
                    try {
                        connect();
                        worker.updateContext(
                                snapshot, baseband, safe && epoch == contextEpoch && contextSafe);
                    } catch (Exception e) {
                        contextSafe = false;
                    }
                });
    }

    private String token() {
        return UUID.randomUUID().toString().replace("-", "");
    }

    private void renewContext(long epoch) throws Exception {
        final String[] snapshot = home.clone();
        if (epoch != contextEpoch || epoch != state.epoch())
            throw new IllegalStateException("identity unavailable");
        // Complete renewal before dispatching an EFS call. The heartbeat still
        // renews/revokes during long calls through the loader's real Binder pool.
        worker.updateContext(snapshot, SystemProperties.get("gsm.version.baseband", ""),
                contextSafe && epoch == contextEpoch && epoch == state.epoch());
    }

    private static String readBootId() {
        try (BufferedReader reader =
                new BufferedReader(new FileReader("/proc/sys/kernel/random/boot_id"))) {
            String value = reader.readLine();
            return value == null ? "" : value.trim();
        } catch (Exception unavailable) {
            return "";
        }
    }

    private void coordinate() {
        final long epoch = contextEpoch;
        final long version = configVersion;
        final String identity = state.identity();
        final int loadedSimCount = home.length;
        try {
            boolean enabled = prefs.getBoolean("automatic", true);
            if (!CoordinatorState.shouldCoordinate(enabled, loadedSimCount)) return;
            if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime())) return;
            connect();
            renewContext(epoch);
            McfgStatus status = worker.getStatus();
            if (version != configVersion) return;
            // Recovery does not require a carrier candidate. Dispatch before
            // initialization or carrier selection starts a fresh attempt.
            if (CoordinatorState.shouldRecover(status.state, contextSafe && epoch == contextEpoch
                    && epoch == state.epoch(), status.writesAllowed)) {
                if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime())) return;
                renewContext(epoch);
                status = worker.recover();
                publish(status);
                verifiedEpoch = -1;
                restoredEpoch = -1;
                return;
            }
            if (configuredVersion != version) {
                if (enabled) {
                    // Startup/reconnect must preserve an already enabled error.
                    // Explicit switch requests still start a fresh attempt.
                    if (CoordinatorState.shouldInitializeAutomatic(status.enabled,
                            version != prefs.getLong("configured_version", 0) ? 1 : 0,
                            status.state, status.profileId)) {
                        renewContext(epoch);
                        status = worker.setEnabled(true);
                    }
                } else if (status.enabled && !prefs.getBoolean("disable_requested", false))
                    prefs.edit().putBoolean("disable_requested", true).commit();
                if (!enabled) {
                    renewContext(epoch);
                    status = worker.setEnabled(false);
                }
                configuredVersion = version;
                prefs.edit().putLong("configured_version", version).commit();
            }
            publish(status);
            if (!contextSafe
                    || epoch != contextEpoch
                    || epoch != state.epoch()
                    || !status.writesAllowed) return;
            if (!enabled && (restoreRequested || prefs.getBoolean("disable_requested", false))) {
                // The durable request survives process death and reboot.
                if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime())) return;
                renewContext(epoch);
                status = worker.restore(token());
                restoreRequested = false;
                prefs.edit().putBoolean("disable_requested", false).commit();
                publish(status);
            }
            if (enabled
                    && prefs.getBoolean("automatic", true)
                    && !prefs.getBoolean("disable_requested", false)
                    && !restoreRequested) {
                if (!status.enabled
                        && status.profileId.isEmpty()
                        && ("disabled".equals(status.state) || "error".equals(status.state))) {
                    // A completed stock restore must not turn off automatic SIM selection.
                    renewContext(epoch);
                    status = worker.setEnabled(true);
                    publish(status);
                }
                if (restoredEpoch != epoch
                        && status.candidateProfileId.isEmpty()
                        && CoordinatorState.shouldRestoreAutomatic(status.state, loadedSimCount,
                                !status.profileId.isEmpty(), status.autoselect)) {
                    if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime())) return;
                    renewContext(epoch);
                    status = worker.restore(token());
                    restoredEpoch = epoch;
                    publish(status);
                }
            }
            if ("restart_pending".equals(status.state)) {
                // Every transaction needs its existing token-claimed restart.
                // Ignore the obsolete restart preference in older app settings.
                if (enabled
                        && !status.profileId.isEmpty()
                        && !status.candidateProfileId.equals(status.profileId)) {
                    if (status.candidateProfileId.isEmpty()) {
                        if (!status.profileId.isEmpty())
                            return; // Mixed/unknown carriers: no writes.
                    } else {
                        if (CoordinatorState.needsApply(verifiedEpoch, verifiedConfigVersion, epoch, version,
                                status.state, status.profileId, status.sourceName, status.candidateProfileId)) {
                            if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime()))
                                return;
                            renewContext(epoch);
                            McfgStatus result = worker.apply(token());
                            publish(result);
                            boolean applied = CoordinatorState.shouldRecordApplied(
                                    result.state, result.profileId, result.sourceName);
                            if (applied) recordApplied(epoch, version);
                        }
                        return;
                    }
                }
                if (!status.restartAttempted) {
                    // The heartbeat renews the vendor worker's 5-second context lease.
                    if (!contextSafe || epoch != contextEpoch) return;
                    final String restartToken = status.transactionToken;
                    // Reserve on main so radio callbacks observe the token before the
                    // loader triggers the helper. Keep all operations serial.
                    FutureTask<Void> restart =
                            new FutureTask<>(
                                    () -> {
                                        boolean reserved = false;
                                        try {
                                            List<SubscriptionInfo> active =
                                                    CarrierSettingsFragment.presentSubscriptions(this);
                                            if (!contextSafe
                                                    || epoch != contextEpoch
                                                    || !CoordinatorState.shouldCoordinate(
                                                            prefs.getBoolean("automatic", true), home.length)
                                                    || active == null) {
                                                // Leave the token available until the identity is known.
                                                return null;
                                            }
                                            prefs.edit()
                                                    .putString("restart_token", restartToken)
                                                    .putString("restart_identity", identity)
                                                    .putString("restart_boot_id", bootId)
                                                    .putBoolean("restart_saw_down", false)
                                                    .putString(
                                                            "restart_note",
                                                            "Restart reserved; waiting for modem recovery")
                                                    .commit();
                                            reserved = true;
                                            renewContext(epoch);
                                            worker.claimRestart(restartToken);
                                        } catch (Exception e) {
                                            if (!reserved)
                                                return null; // Lease/context changes simply defer reservation.
                                            prefs.edit()
                                                    .putString(
                                                            "restart_note",
                                                            "Modem reboot unavailable; manual restart required")
                                                    .commit();
                                            report(
                                                    "Restart pending: modem reboot unavailable; no automatic retry");
                                        }
                                        return null;
                                    });
                    main.post(restart);
                    restart.get();
                } else if (state.canComplete(
                                prefs.getBoolean("restart_saw_down", false),
                                prefs.getString("restart_identity", ""), status.profileId.isEmpty(),
                                status.transactionToken, prefs.getString("restart_token", ""))
                        && contextSafe
                        && epoch == contextEpoch) {
                    renewContext(epoch);
                    McfgStatus result = worker.complete(status.transactionToken);
                    publish(result);
                    if (CoordinatorState.shouldRecordApplied(result.state, result.profileId, result.sourceName)) {
                        recordApplied(epoch, version);
                    }
                    prefs.edit()
                            .remove("restart_token")
                            .remove("restart_identity")
                            .remove("restart_boot_id")
                            .remove("restart_saw_down")
                            .remove("restart_note")
                            .commit();
                }
                return;
            }
            if (!enabled) return;
            if (CoordinatorState.needsApply(verifiedEpoch, verifiedConfigVersion, epoch, version,
                            status.state, status.profileId, status.sourceName, status.candidateProfileId)
                    && !status.candidateProfileId.isEmpty()
                    && !"conflict".equals(status.state)) {
                if (!state.retryAllowed(epoch, version, SystemClock.elapsedRealtime())) return;
                renewContext(epoch);
                McfgStatus result = worker.apply(token());
                publish(result);
                // Acknowledge only this attempt's configuration; the journal/candidate
                // comparison handles a SIM change on the next tick.
                boolean applied = CoordinatorState.shouldRecordApplied(
                        result.state, result.profileId, result.sourceName);
                if (applied) recordApplied(epoch, version);
            }
        } catch (Exception e) {
            // Failed writes remain retryable; never change the user's switch.
            verifiedEpoch = -1;
            restoredEpoch = -1;
            // Keep configuration acknowledged: setEnabled(true) would erase
            // the journaled error on the next heartbeat before the retry is due.
            try {
                publish(worker.getStatus());
            } catch (Exception unavailable) {
                report("Carrier profile service unavailable");
            }
            if (version == configVersion)
                state.attemptFailed(epoch, version, SystemClock.elapsedRealtime());
        } finally {
            busy.set(false);
            main.post(this::updateHeartbeat);
        }
    }

    static void requestConfiguration(SharedPreferences prefs) {
        synchronized (prefs) {
            prefs.edit().putLong("config_version", prefs.getLong("config_version", 0) + 1).commit();
        }
    }

    private void recordApplied(long epoch, long version) {
        verifiedEpoch = epoch;
        verifiedConfigVersion = version;
        prefs.edit().putLong("verified_config_version", version).commit();
    }

    private void publish(McfgStatus status) {
        lastStatus = status;
        String text = status.state + (status.profileId.isEmpty() ? "" : " — " + status.profileId);
        if ("restart_pending".equals(status.state) && status.profileId.isEmpty())
            text += "\n" + getString(R.string.normalizing);
        if (!status.firmwareGeneration.isEmpty())
            text += "\nFirmware: " + status.firmwareGeneration;
        if (!status.profileId.isEmpty()) {
            text +=
                    "\n"
                            + getString(
                                    status.profileId.startsWith("imported/")
                                            ? R.string.imported
                                            : R.string.bundled);
            if (!status.sourceName.isEmpty()) text += "\n" + status.sourceName;
            if (!status.sourceSha256.isEmpty()) text += "\nSHA-256: " + status.sourceSha256;
        }
        if ("restart_pending".equals(status.state))
            text += "\n" + prefs.getString("restart_note", "Waiting for a safe modem restart");
        if (!status.detail.isEmpty()) text += "\n" + status.detail;
        report(text);
    }

    private void report(String text) {
        prefs.edit().putString("status", text).apply();
    }

    private final class SlotCallback extends TelephonyCallback
            implements TelephonyCallback.RadioPowerStateListener {
        final TelephonyManager manager;
        boolean radioOn;

        SlotCallback(TelephonyManager manager) {
            this.manager = manager;
        }

        @Override
        public void onRadioPowerStateChanged(int state) {
            radioOn = state == TelephonyManager.RADIO_POWER_ON;
            if (!radioOn) {
                // Invalidate before recording the down witness: SIM LOADED may lag SSR.
                McfgCoordinatorService.this.state.setRadiosReady(false);
                contextSafe = false;
            }
            if (CoordinatorState.observeRadio(radioOn, prefs.getString("restart_token", ""),
                    prefs.getBoolean("restart_saw_down", false)))
                prefs.edit().putBoolean("restart_saw_down", true).commit();
            tick();
        }
    }
}
