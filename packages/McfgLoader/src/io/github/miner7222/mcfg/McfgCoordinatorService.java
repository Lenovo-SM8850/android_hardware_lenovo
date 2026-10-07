// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.telephony.ServiceState;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.telephony.emergency.EmergencyNumber;

import vendor.lenovo.hardware.mcfg.IMcfgLoader;
import vendor.lenovo.hardware.mcfg.McfgStatus;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class McfgCoordinatorService extends Service {
    static final String NAME = "vendor.lenovo.hardware.mcfg.IMcfgLoader/default";
    static final String PROBE = "io.github.miner7222.mcfg.PROBE";
    static final String RESTORE = "io.github.miner7222.mcfg.RESTORE";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService transactions = Executors.newSingleThreadExecutor();
    private final ExecutorService contextIpc = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean();
    private final AtomicBoolean restartQueued = new AtomicBoolean();
    private final Map<Integer, SlotCallback> callbacks = new HashMap<>();
    private final Set<String> domainEmergencyModes = new HashSet<>();
    private volatile IMcfgLoader worker;
    private volatile String[] home = new String[0];
    private volatile long contextEpoch;
    private volatile long verifiedEpoch = -1;
    private volatile boolean contextSafe;
    private SharedPreferences prefs;
    private SubscriptionManager subscriptions;
    private final CoordinatorState state = new CoordinatorState();
    private volatile boolean failedAttempt, readyForCompletion, configDirty = true;
    private volatile boolean restoreRequested;
    private long nextReadback;
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    invalidate();
                    tick();
                }
            };
    private final Runnable heartbeat =
            new Runnable() {
                @Override
                public void run() {
                    tick();
                    main.postDelayed(this, 1000);
                }
            };

    @Override
    public void onCreate() {
        super.onCreate();
        prefs = createDeviceProtectedStorageContext().getSharedPreferences("mcfg", MODE_PRIVATE);
        subscriptions = getSystemService(SubscriptionManager.class);
        subscriptions.addOnSubscriptionsChangedListener(getMainExecutor(), subListener);
        NotificationManager manager = getSystemService(NotificationManager.class);
        // Remove the status notification and channel left by earlier versions.
        manager.cancel(1);
        manager.deleteNotificationChannel("mcfg");
        main.post(heartbeat);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        configDirty = true;
        if (prefs.getBoolean("automatic", true)) failedAttempt = false;
        if (intent != null && PROBE.equals(intent.getAction())) {
            transactions.execute(
                    () -> {
                        try {
                            connect();
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
            failedAttempt = false;
        }
        invalidate();
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
        subscriptions.removeOnSubscriptionsChangedListener(subListener);
        for (SlotCallback cb : callbacks.values()) cb.manager.unregisterTelephonyCallback(cb);
        transactions.shutdown();
        contextIpc.shutdown();
        super.onDestroy();
    }

    private void invalidate() {
        state.invalidate(SystemClock.elapsedRealtime());
        contextEpoch = state.epoch();
        contextSafe = false;
        readyForCompletion = false;
        sendContext();
    }

    private void emergency() {
        // A callback without an exit snapshot cannot authorize writes. Hold off
        // for emergency SMS/call setup, then query all emergency modes again.
        state.emergency(SystemClock.elapsedRealtime());
        invalidate();
    }

    private static IBinder binder(String name) throws Exception {
        return (IBinder)
                Class.forName("android.os.ServiceManager")
                        .getMethod("checkService", String.class)
                        .invoke(null, name);
    }

    private void connect() throws Exception {
        if (worker != null && worker.asBinder().isBinderAlive()) return;
        IBinder binder = binder(NAME);
        if (binder == null) throw new IllegalStateException("loader unavailable");
        worker = IMcfgLoader.Stub.asInterface(binder);
        binder.linkToDeath(
                () -> {
                    worker = null;
                    main.post(this::invalidate);
                },
                0);
    }

    private Object service(String name, String stub) throws Exception {
        IBinder binder = binder(name);
        if (binder == null || !binder.isBinderAlive())
            throw new IllegalStateException("telephony unavailable");
        return Class.forName(stub).getMethod("asInterface", IBinder.class).invoke(null, binder);
    }

    private Object query(Object remote, String stub, String name, Class<?>[] types, Object... args)
            throws Exception {
        // Invoke the actual Binder interface, not TelephonyManager getters that
        // can turn a RemoteException into an apparently safe false/IDLE value.
        Method method = Class.forName(stub).getMethod(name, types);
        return method.invoke(remote, args);
    }

    private boolean safeSnapshot(List<SubscriptionInfo> active) throws Exception {
        if (!domainEmergencyModes.isEmpty()) return false;
        if (!(boolean)
                Class.forName("com.android.internal.telephony.flags.Flags")
                        .getMethod("domainSelectionEmergencyModeNotification")
                        .invoke(null)) return false;
        String phoneStub = "com.android.internal.telephony.ITelephony";
        Object phone = service("phone", phoneStub + "$Stub");
        Object telecom = service("telecom", "com.android.internal.telecom.ITelecomService$Stub");
        if ((boolean)
                        query(
                                telecom,
                                "com.android.internal.telecom.ITelecomService",
                                "isInEmergencyCall",
                                new Class<?>[0])
                || (boolean)
                        query(
                                telecom,
                                "com.android.internal.telecom.ITelecomService",
                                "isInCall",
                                new Class<?>[] {String.class, String.class},
                                getPackageName(),
                                null)
                || (boolean) query(phone, phoneStub, "isInEmergencySmsMode", new Class<?>[0]))
            return false;
        for (SubscriptionInfo info : active) {
            int id = info.getSubscriptionId();
            SlotCallback callback = callbacks.get(id);
            if (callback == null
                    || !callback.callbackMode.knownAndClear()
                    || !callback.domainMode.knownAndClear()) return false;
            if ((int)
                                    query(
                                            phone,
                                            phoneStub,
                                            "getCallStateForSubscription",
                                            new Class<?>[] {int.class, String.class, String.class},
                                            id,
                                            getPackageName(),
                                            null)
                            != TelephonyManager.CALL_STATE_IDLE
                    || (boolean)
                            query(
                                    phone,
                                    phoneStub,
                                    "getEmergencyCallbackMode",
                                    new Class<?>[] {int.class},
                                    id)
                    || !(boolean)
                            query(
                                    phone,
                                    phoneStub,
                                    "isRadioOnForSubscriberWithFeature",
                                    new Class<?>[] {int.class, String.class, String.class},
                                    id,
                                    getPackageName(),
                                    null)) return false;
        }
        return true;
    }

    private void syncCallbacks(List<SubscriptionInfo> active) {
        List<Integer> ids = new ArrayList<>();
        for (SubscriptionInfo info : active) ids.add(info.getSubscriptionId());
        for (int id : new ArrayList<>(callbacks.keySet()))
            if (!ids.contains(id)) {
                SlotCallback cb = callbacks.remove(id);
                cb.manager.unregisterTelephonyCallback(cb);
            }
        for (int id : ids)
            if (!callbacks.containsKey(id)) {
                SlotCallback cb =
                        new SlotCallback(
                                getSystemService(TelephonyManager.class)
                                        .createForSubscriptionId(id));
                cb.manager.registerTelephonyCallback(getMainExecutor(), cb);
                callbacks.put(id, cb);
            }
    }

    private void tick() {
        try {
            List<SubscriptionInfo> active = subscriptions.getActiveSubscriptionInfoList();
            if (active == null || active.size() > 2)
                throw new IllegalStateException("identity unavailable");
            active.sort(Comparator.comparingInt(SubscriptionInfo::getSimSlotIndex));
            syncCallbacks(active);
            List<String> operators = new ArrayList<>();
            StringBuilder key = new StringBuilder();
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
                key.append(info.getSubscriptionId()).append(':').append(operator).append(';');
            }
            String nextIdentity = active.isEmpty() ? "no_sim" : key.toString();
            if (state.observe(nextIdentity, SystemClock.elapsedRealtime())) {
                home = operators.toArray(new String[0]);
                failedAttempt = false;
                invalidate();
            }
            boolean safe = false;
            try {
                safe = state.stable(SystemClock.elapsedRealtime()) && safeSnapshot(active);
            } catch (Exception unavailable) {
                // Unknown call/emergency state revokes the write lease, not the
                // loaded SIM's home identity used for read-only profile selection.
            }
            contextSafe = safe;
            if (!safe) readyForCompletion = false;
            else
                readyForCompletion =
                        state.canComplete(
                                SystemClock.elapsedRealtime(),
                                prefs.getBoolean("restart_saw_down", false));
            sendContext();
            if (!busy.getAndSet(true)) transactions.execute(this::coordinate);
        } catch (Exception e) {
            home = new String[0];
            state.clear(SystemClock.elapsedRealtime());
            invalidate();
            report("Waiting for loaded SIMs and known call/emergency state");
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

    private void coordinate() {
        try {
            connect();
            boolean enabled = prefs.getBoolean("automatic", true);
            McfgStatus status = worker.getStatus();
            if (configDirty) {
                configDirty = false;
                verifiedEpoch = -1;
                if (enabled) status = worker.setEnabled(true);
                else if (status.enabled && !prefs.getBoolean("disable_requested", false))
                    prefs.edit().putBoolean("disable_requested", true).commit();
                if (!enabled) status = worker.setEnabled(false);
            }
            publish(status);
            if (!contextSafe || !status.writesAllowed) return;
            if (!enabled && (restoreRequested || prefs.getBoolean("disable_requested", false))) {
                // The durable request survives calls, process death and reboot.
                status = worker.restore(token());
                restoreRequested = false;
                prefs.edit().putBoolean("disable_requested", false).commit();
                publish(status);
            }
            if ("restart_pending".equals(status.state)) {
                // OFF is the persistent app disable/restore request and needs
                // its restart. The optional switch governs automatic overlays.
                if (!status.profileId.isEmpty() && !prefs.getBoolean("restart", true)) return;
                if (enabled && !status.candidateProfileId.equals(status.profileId)) {
                    if (status.candidateProfileId.isEmpty()) {
                        if (!status.profileId.isEmpty())
                            return; // Mixed/unknown carriers: no writes.
                    } else {
                        publish(worker.apply(token()));
                        return;
                    }
                }
                if (!status.restartAttempted) {
                    // Fresh live checks run on the main thread's heartbeat; a
                    // lease expires after 5 seconds in the vendor worker.
                    if (!contextSafe || !restartQueued.compareAndSet(false, true)) return;
                    final String restartToken = status.transactionToken;
                    final boolean restoringAutoselect = status.profileId.isEmpty();
                    main.post(
                            () -> {
                                boolean reserved = false;
                                try {
                                    List<SubscriptionInfo> active =
                                            subscriptions.getActiveSubscriptionInfoList();
                                    if (!contextSafe
                                            || (!restoringAutoselect
                                                    && !prefs.getBoolean("restart", true))
                                            || active == null
                                            || !safeSnapshot(active)) {
                                        return; // Do not consume the at-most-once token while
                                                // unsafe.
                                    }
                                    if (!worker.claimRestart(restartToken)) return;
                                    reserved = true;
                                    prefs.edit()
                                            .putString("restart_token", restartToken)
                                            .putBoolean("restart_saw_down", false)
                                            .putString(
                                                    "restart_note",
                                                    "Restart reserved; waiting for modem recovery")
                                            .commit();
                                    contextSafe = false;
                                    sendContext();
                                    state.restarting(SystemClock.elapsedRealtime());
                                    TelephonyManager modem =
                                            getSystemService(TelephonyManager.class);
                                    if (!active.isEmpty())
                                        modem =
                                                modem.createForSubscriptionId(
                                                        active.get(0).getSubscriptionId());
                                    modem.rebootModem();
                                    invalidate();
                                } catch (Exception e) {
                                    if (!reserved)
                                        return; // Lease/context changes simply defer reservation.
                                    failedAttempt = true;
                                    prefs.edit()
                                            .putString(
                                                    "restart_note",
                                                    "Modem reboot unavailable; manual restart required")
                                            .commit();
                                    report(
                                            "Restart pending: modem reboot unavailable; no automatic retry");
                                } finally {
                                    restartQueued.set(false);
                                }
                            });
                } else if (readyForCompletion
                        && status.transactionToken.equals(prefs.getString("restart_token", ""))
                        && prefs.getBoolean("restart_saw_down", false)) {
                    McfgStatus result = worker.complete(status.transactionToken);
                    publish(result);
                    verifiedEpoch = contextEpoch;
                    prefs.edit()
                            .remove("restart_token")
                            .remove("restart_saw_down")
                            .remove("restart_note")
                            .commit();
                }
                return;
            }
            if (!enabled) return;
            if (failedAttempt || "rollback_failed".equals(status.state)) return;
            if ((verifiedEpoch != contextEpoch || SystemClock.elapsedRealtime() >= nextReadback)
                    && !status.candidateProfileId.isEmpty()
                    && !"conflict".equals(status.state)) {
                McfgStatus result = worker.apply(token());
                publish(result);
                nextReadback = SystemClock.elapsedRealtime() + 30_000;
                if ("no_change".equals(result.state)) verifiedEpoch = contextEpoch;
            }
        } catch (Exception e) {
            try {
                McfgStatus result = worker.getStatus();
                // Unsafe/expired leases and a service still starting defer work.
                // A verified rollback or recovery failure remains visibly latched.
                if (!result.enabled
                        && ("error".equals(result.state)
                                || "rollback_failed".equals(result.state))) {
                    failedAttempt = true;
                    prefs.edit().putBoolean("automatic", false).commit();
                } else {
                    configDirty = true;
                }
                publish(result);
            } catch (Exception unavailable) {
                configDirty = true;
                report("Carrier profile service unavailable");
            }
        } finally {
            busy.set(false);
        }
    }

    private void publish(McfgStatus status) {
        String text = status.state + (status.profileId.isEmpty() ? "" : " — " + status.profileId);
        if (!status.writesAllowed) text += "\n" + getString(R.string.readonly);
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
            implements TelephonyCallback.CallStateListener,
                    TelephonyCallback.ServiceStateListener,
                    TelephonyCallback.RadioPowerStateListener,
                    TelephonyCallback.OutgoingEmergencyCallListener,
                    TelephonyCallback.OutgoingEmergencySmsListener,
                    TelephonyCallback.EmergencyCallbackModeListener,
                    TelephonyCallback.DomainSelectionEmergencyModeListener {
        final TelephonyManager manager;
        final CoordinatorState.EmergencySnapshot callbackMode =
                new CoordinatorState.EmergencySnapshot();
        final CoordinatorState.EmergencySnapshot domainMode =
                new CoordinatorState.EmergencySnapshot();

        SlotCallback(TelephonyManager manager) {
            this.manager = manager;
        }

        @Override
        public void onCallStateChanged(int state) {
            if (state != TelephonyManager.CALL_STATE_IDLE) invalidate();
        }

        @Override
        public void onServiceStateChanged(ServiceState state) {
            invalidate();
        }

        @Override
        public void onRadioPowerStateChanged(int state) {
            if (state != TelephonyManager.RADIO_POWER_ON
                    && !prefs.getString("restart_token", "").isEmpty())
                prefs.edit().putBoolean("restart_saw_down", true).commit();
            invalidate();
        }

        @Override
        public void onOutgoingEmergencyCall(EmergencyNumber number, int id) {
            emergency();
        }

        @Override
        public void onOutgoingEmergencySms(EmergencyNumber number, int id) {
            emergency();
        }

        @Override
        public void onCallbackModeStarted(int type, Duration remaining, int id) {
            callbackMode.update(type, true);
            emergency();
        }

        @Override
        public void onCallbackModeRestarted(int type, Duration remaining, int id) {
            callbackMode.update(type, true);
            emergency();
        }

        @Override
        public void onCallbackModeStopped(int type, int reason, int id) {
            callbackMode.update(type, false);
            invalidate();
        }

        @Override
        public void onDomainSelectionEmergencyModeEntered(int type, int slot, int id) {
            domainMode.update(type, true);
            domainEmergencyModes.add(type + ":" + slot);
            emergency();
        }

        @Override
        public void onDomainSelectionEmergencyModeExited(int type, int slot, int id) {
            domainMode.update(type, false);
            domainEmergencyModes.remove(type + ":" + slot);
            invalidate();
        }
    }
}
