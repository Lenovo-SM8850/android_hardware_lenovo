// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.app.Activity;
import android.content.Intent;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.UserHandle;
import android.provider.OpenableColumns;
import android.telephony.SubscriptionInfo;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import android.view.View;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceGroup;

import com.android.settingslib.widget.FooterPreference;
import com.android.settingslib.widget.MainSwitchPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;
import com.android.settingslib.widget.TopIntroPreference;

import vendor.lenovo.hardware.mcfg.IMcfgLoader;
import vendor.lenovo.hardware.mcfg.McfgStatus;
import vendor.lenovo.hardware.mcfg.ProfileEntry;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;

public final class CarrierSettingsFragment extends SettingsBasePreferenceFragment {
    private static final int PICK_MBN = 1;
    private int subId, importSubId;
    private String importHome = "";
    private SharedPreferences prefs;
    private Context appContext;
    private SubscriptionManager subscriptions;
    private boolean listening, resumed, preferencesAlive;
    private int viewGeneration;
    private Preference status;
    private PreferenceCategory imported;
    private Preference overridesEntry;
    private Preference importAction;
    private boolean callIdle, transactionRunning = true, working;
    private McfgStatus lastStatus;
    private AlertDialog importDialog;
    private String importsHome = "";
    private final java.util.Map<Preference, ProfileEntry> importRows = new java.util.HashMap<>();
    private MainSwitchPreference automatic;
    private ListPreference sim;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    // Status polling has its own thread so a slow loader call never delays imports.
    private final ExecutorService poller = Executors.newSingleThreadExecutor();
    private final AtomicBoolean polling = new AtomicBoolean();
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    if (!resumed || !attached()) return;
                    updateSubscriptions();
                }
            };
    private final Runnable refresh =
            new Runnable() {
                @Override
                public void run() {
                    if (!resumed || !attached()) return;
                    pollStatus();
                    automatic.setChecked(prefs.getBoolean("automatic", false));
                    overridesEntry.setEnabled(active());
                    updateImportActions();
                    main.postDelayed(this, 1000);
                }
            };

    private boolean active() {
        return attached() && active(appContext, subId);
    }

    private boolean attached() {
        return preferencesAlive && isAdded() && getContext() != null;
    }

    @Override
    public void onAttach(Context context) {
        super.onAttach(context);
        appContext = context.getApplicationContext();
        subscriptions = appContext.getSystemService(SubscriptionManager.class);
    }

    @Override
    public void onViewCreated(View view, Bundle saved) {
        super.onViewCreated(view, saved);
        preferencesAlive = true;
        ++viewGeneration;
    }

    static boolean active(android.content.Context context, int subId) {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return false;
        for (SubscriptionInfo info : presentSubscriptions(context))
            if (info.getSubscriptionId() == subId) return true;
        return false;
    }

    static java.util.List<SubscriptionInfo> presentSubscriptions(android.content.Context context) {
        SubscriptionManager subscriptions = context.getSystemService(SubscriptionManager.class);
        TelephonyManager phones = context.getSystemService(TelephonyManager.class);
        java.util.List<SubscriptionInfo> active = subscriptions.getActiveSubscriptionInfoList();
        java.util.Map<Integer, SubscriptionInfo> slots = new java.util.TreeMap<>();
        if (active != null) {
            for (SubscriptionInfo info : active) {
                int slot = info.getSimSlotIndex();
                // During a swap the list can lag the current logical-slot mapping.
                // One present subscription per slot excludes old IDs and duplicates.
                if (CoordinatorState.isCurrentSubscription(info.getSubscriptionId(), slot,
                        SubscriptionManager.getSubscriptionId(slot),
                        slot >= 0 && phones.hasIccCard(slot))
                        && subscriptions.isActiveSubscriptionId(info.getSubscriptionId()))
                    slots.put(slot, info);
            }
        }
        return new java.util.ArrayList<>(slots.values());
    }

    /** Keeps {@code current} while it is active; otherwise the default data SIM or the first SIM. */
    static int resolveSubscription(android.content.Context context, int current) {
        java.util.List<SubscriptionInfo> list = presentSubscriptions(context);
        return resolveSubscription(list, current);
    }

    private static int resolveSubscription(java.util.List<SubscriptionInfo> list, int current) {
        for (SubscriptionInfo info : list)
            if (info.getSubscriptionId() == current) return current;
        int defaultSub = SubscriptionManager.getDefaultDataSubscriptionId();
        for (SubscriptionInfo info : list)
            if (info.getSubscriptionId() == defaultSub) return defaultSub;
        return list.isEmpty()
                ? SubscriptionManager.INVALID_SUBSCRIPTION_ID
                : list.get(0).getSubscriptionId();
    }

    private void pollStatus() {
        if (!resumed || !attached()) return;
        final int generation = viewGeneration;
        if (!polling.compareAndSet(false, true)) return;
        try {
            poller.execute(
                    () -> {
                        McfgStatus loaded;
                        try {
                            loaded = worker().getStatus();
                        } catch (Exception unavailable) {
                            loaded = null;
                        }
                        final McfgStatus snapshot = loaded;
                        polling.set(false);
                        main.post(
                                () -> {
                                    if (resumed && attached() && generation == viewGeneration) {
                                        lastStatus = snapshot;
                                        transactionRunning = snapshot == null
                                                || !snapshot.transactionToken.isEmpty();
                                        updateImportActions();
                                        status.setSummary(
                                                snapshot != null
                                                        ? statusText(snapshot)
                                                        : getString(R.string.waiting));
                                    }
                                });
                    });
        } catch (RejectedExecutionException shutDown) {
            polling.set(false);
        }
    }

    /** Maps durable loader progress and applied sources to the status text. */
    private String statusText(McfgStatus loader) {
        String mapped = CoordinatorState.displayState(loader.state, loader.profileId,
                loader.sourceName, loader.candidateProfileId,
                prefs.getBoolean("automatic", false), loader.enabled);
        String text =
                getString(
                        "baseline".equals(mapped)
                                ? R.string.baseline_restart
                                : "waiting".equals(mapped)
                                ? R.string.waiting
                                : "error".equals(mapped)
                                ? R.string.status_error
                                : "applied".equals(mapped)
                                        ? R.string.status_applied
                                        : R.string.status_default);
        if ("applied".equals(mapped))
            text += "\n" + loader.sourceName;
        return text;
    }

    private static IMcfgLoader worker() throws Exception {
        android.os.IBinder binder =
                (android.os.IBinder)
                        Class.forName("android.os.ServiceManager")
                                .getMethod("checkService", String.class)
                                .invoke(null, McfgCoordinatorService.NAME);
        if (binder == null) throw new IllegalStateException("Carrier profile service unavailable");
        return IMcfgLoader.Stub.asInterface(binder);
    }

    @Override
    public void onCreatePreferences(Bundle saved, String rootKey) {
        preferencesAlive = true;
        ++viewGeneration;
        getPreferenceManager().setStorageDeviceProtected();
        prefs =
                requireContext()
                        .createDeviceProtectedStorageContext()
                        .getSharedPreferences("mcfg", 0);
        subId = getArguments().getInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, -1);
        if (saved != null) {
            subId = saved.getInt("selected_sub", subId);
            importSubId = saved.getInt("import_sub", -1);
            importHome = saved.getString("import_home", "");
        }
        setPreferenceScreen(getPreferenceManager().createPreferenceScreen(requireContext()));
        TopIntroPreference intro = new TopIntroPreference(requireContext());
        intro.setTitle(R.string.description);
        getPreferenceScreen().addPreference(intro);
        sim = new ListPreference(requireContext());
        sim.setKey("sim");
        sim.setTitle(R.string.sim);
        sim.setPersistent(false);
        updateSubscriptions();
        sim.setSummaryProvider(ListPreference.SimpleSummaryProvider.getInstance());
        sim.setOnPreferenceChangeListener(
                (p, value) -> {
                    subId = Integer.parseInt(value.toString());
                    getArguments().putInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId);
                    reloadImports();
                    return true;
                });
        getPreferenceScreen().addPreference(sim);
        automatic = new MainSwitchPreference(requireContext());
        automatic.setTitle(R.string.automatic);
        automatic.setPersistent(false);
        automatic.setChecked(prefs.getBoolean("automatic", false));
        automatic.setEnabled(UserHandle.myUserId() == UserHandle.USER_SYSTEM);
        automatic.setOnPreferenceChangeListener(
                (p, value) -> {
                    boolean enabled = (Boolean) value;
                    prefs.edit()
                            .putBoolean("automatic", enabled)
                            .putBoolean("disable_requested", !enabled)
                            .commit();
                    if (!enabled) status.setSummary(R.string.status_default);
                    service(enabled ? McfgCoordinatorService.ENABLE : McfgCoordinatorService.RESTORE);
                    return true;
                });
        getPreferenceScreen().addPreference(automatic);
        status = action(getPreferenceScreen(), R.string.notice, () -> {});
        status.setSelectable(false);
        importAction = action(
                getPreferenceScreen(),
                R.string.import_mbn,
                () -> {
                    if (!active()) {
                        error(getString(R.string.select_sim));
                        return;
                    }
                    importSubId = subId;
                    importHome =
                            requireContext()
                                    .getSystemService(TelephonyManager.class)
                                    .createForSubscriptionId(subId)
                                    .getSimOperator();
                    startActivityForResult(
                            new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                    .setType("*/*")
                                    .addCategory(Intent.CATEGORY_OPENABLE),
                            PICK_MBN);
                });
        overridesEntry =
                action(
                        getPreferenceScreen(),
                        R.string.carrier_overrides,
                        () -> {
                            Bundle args = new Bundle();
                            args.putInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId);
                            CarrierOverridesFragment next = new CarrierOverridesFragment();
                            next.setArguments(args);
                            requireActivity()
                                    .getSupportFragmentManager()
                                    .beginTransaction()
                                    .replace(
                                            com.android.settingslib.collapsingtoolbar.R.id
                                                    .content_frame,
                                            next)
                                    .addToBackStack(null)
                                    .commit();
                        });
        overridesEntry.setEnabled(active());
        imported = category(R.string.imported_profiles);
        imported.setVisible(false);
        reloadImports();
        FooterPreference footer = new FooterPreference(requireContext());
        footer.setTitle(getString(R.string.profile_limits));
        getPreferenceScreen().addPreference(footer);
    }

    void setSwitchesEnabled(boolean idle) {
        if (!attached() || automatic == null) return;
        callIdle = idle;
        boolean enabled = idle && UserHandle.myUserId() == UserHandle.USER_SYSTEM;
        automatic.setEnabled(enabled);
        updateImportActions();
    }

    private void updateImportActions() {
        if (imported == null || importAction == null || !attached()) return;
        boolean enabled = callIdle && !transactionRunning && !working && active();
        importAction.setEnabled(enabled);
        String currentHome = requireContext().getSystemService(TelephonyManager.class)
                .createForSubscriptionId(subId).getSimOperator();
        boolean importsEnabled = enabled && !currentHome.isEmpty() && importsHome.equals(currentHome);
        imported.setEnabled(importsEnabled);
        if (importDialog != null && importDialog.isShowing()) {
            importDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(importsEnabled);
            importDialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(importsEnabled);
        }
        imported.setVisible(!importRows.isEmpty() && importsHome.equals(currentHome)
                && active());
        for (java.util.Map.Entry<Preference, ProfileEntry> row : importRows.entrySet()) {
            ProfileEntry entry = row.getValue();
            boolean chosen = entry.chosenHomePlmns != null
                    && Arrays.asList(entry.chosenHomePlmns).contains(importsHome);
            boolean applied = lastStatus != null && entry.id.equals(lastStatus.profileId)
                    && "applied".equals(CoordinatorState.displayState(lastStatus.state,
                            lastStatus.profileId, lastStatus.sourceName, lastStatus.candidateProfileId,
                            prefs.getBoolean("automatic", false), lastStatus.enabled));
            row.getKey().setSummary(applied ? getString(R.string.import_applied_summary, entry.sourceSha256)
                    : chosen ? getString(R.string.import_chosen_summary, entry.sourceSha256)
                    : entry.sourceSha256);
        }
    }

    private void updateSubscriptions() {
        if (!attached() || sim == null) return;
        java.util.List<SubscriptionInfo> list = presentSubscriptions(appContext);
        String[] labels = new String[list.size()], ids = new String[list.size()];
        for (int i = 0; i < list.size(); i++) {
            labels[i] = list.get(i).getDisplayName().toString();
            ids[i] = Integer.toString(list.get(i).getSubscriptionId());
        }
        int previous = subId;
        subId = resolveSubscription(list, subId);
        getArguments().putInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId);
        sim.setEntries(labels);
        sim.setEntryValues(ids);
        sim.setValue(Integer.toString(subId));
        sim.setEnabled(!list.isEmpty());
        if (imported != null && previous != subId) reloadImports();
    }

    private PreferenceCategory category(int title) {
        PreferenceCategory group = new PreferenceCategory(requireContext());
        group.setTitle(title);
        getPreferenceScreen().addPreference(group);
        return group;
    }

    private Preference action(PreferenceGroup group, int title, Runnable run) {
        Preference p = new Preference(requireContext());
        p.setTitle(title);
        p.setOnPreferenceClickListener(
                preference -> {
                    run.run();
                    return true;
                });
        group.addPreference(p);
        return p;
    }

    private void service(String action) {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return;
        Intent intent = new Intent(appContext, McfgCoordinatorService.class);
        if (action != null) intent.setAction(action);
        appContext.startService(intent);
    }

    private void error(String text) {
        if (attached())
            new AlertDialog.Builder(requireContext())
                    .setTitle(R.string.operation_failed)
                    .setMessage(text)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
    }

    private interface Work {
        void run() throws Exception;
    }

    private void work(Work operation) {
        if (!attached() || io.isShutdown()) return;
        final int generation = viewGeneration;
        working = true;
        updateImportActions();
        io.execute(
                () -> {
                    try {
                        operation.run();
                        // Complete the requested operation even if its UI has gone away.
                        service(null);
                        main.post(
                                () -> {
                                    working = false;
                                    if (attached() && generation == viewGeneration) {
                                        updateImportActions();
                                        reloadImports();
                                    }
                                });
                    } catch (Exception e) {
                        main.post(
                                () -> {
                                    working = false;
                                    if (!attached() || generation != viewGeneration) return;
                                    updateImportActions();
                                    error(e.getMessage() == null
                                            ? e.getClass().getSimpleName() : e.getMessage());
                                });
                    }
                });
    }

    @Override
    public void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("selected_sub", subId);
        out.putInt("import_sub", importSubId);
        out.putString("import_home", importHome);
    }

    @Override
    public void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_MBN
                || result != Activity.RESULT_OK
                || data == null
                || data.getData() == null) return;
        if (!attached()) return;
        final Uri uri = data.getData();
        final int selectedSub = importSubId;
        final String home = importHome;
        final Context context = appContext;
        work(
                () -> {
                    String name = "";
                    try (Cursor cursor =
                            context.getContentResolver()
                                    .query(
                                            uri,
                                            new String[] {OpenableColumns.DISPLAY_NAME},
                                            null,
                                            null,
                                            null)) {
                        if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
                    }
                    if (name == null || !name.toLowerCase(java.util.Locale.ROOT).endsWith(".mbn"))
                        throw new IllegalArgumentException(
                                context.getString(R.string.mbn_required));
                    TelephonyManager sim =
                            context.getSystemService(TelephonyManager.class)
                                    .createForSubscriptionId(selectedSub);
                    if (!context.getSystemService(SubscriptionManager.class)
                                    .isActiveSubscriptionId(selectedSub)
                            || !home.matches("[0-9]{5,6}")
                            || !home.equals(sim.getSimOperator()))
                        throw new IllegalStateException(context.getString(R.string.sim_changed));
                    // SAF may return a pipe. Stage a bounded regular private file; the
                    // worker independently validates and parses its read-only descriptor.
                    File file = File.createTempFile("import-", ".mbn", context.getCacheDir());
                    try {
                        try (InputStream input = context.getContentResolver().openInputStream(uri);
                                FileOutputStream output = new FileOutputStream(file)) {
                            if (input == null)
                                throw new IllegalStateException(
                                        context.getString(R.string.mbn_required));
                            byte[] buffer = new byte[8192];
                            int size = 0, n;
                            while ((n = input.read(buffer)) != -1) {
                                size += n;
                                if (size > 8 * 1024 * 1024)
                                    throw new IllegalArgumentException(
                                            context.getString(R.string.mbn_too_large));
                                output.write(buffer, 0, n);
                            }
                            output.getFD().sync();
                        }
                        if (!home.equals(sim.getSimOperator()))
                            throw new IllegalStateException(
                                    context.getString(R.string.sim_changed));
                        try (ParcelFileDescriptor fd =
                                ParcelFileDescriptor.open(
                                        file, ParcelFileDescriptor.MODE_READ_ONLY)) {
                            worker().importProfile(fd, name, home);
                        }
                    } finally {
                        file.delete();
                    }
                });
    }

    private void reloadImports() {
        if (!attached() || io.isShutdown()) return;
        final int generation = viewGeneration;
        final int selected = subId;
        final Context context = appContext;
        io.execute(
                () -> {
                    try {
                        ProfileEntry[] entries = worker().getProfiles();
                        String home =
                                context.getSystemService(TelephonyManager.class)
                                        .createForSubscriptionId(selected)
                                        .getSimOperator();
                        main.post(
                                () -> {
                                    if (!attached() || generation != viewGeneration || selected != subId
                                            || imported == null) return;
                                    imported.removeAll();
                                    importRows.clear();
                                    importsHome = home;
                                    for (ProfileEntry entry : entries)
                                        if (entry.imported
                                                && Arrays.asList(entry.homePlmns).contains(home)) {
                                            Preference item = new Preference(requireContext());
                                            item.setTitle(entry.sourceName);
                                            item.setSummary(entry.sourceSha256);
                                            item.setOnPreferenceClickListener(
                                                    p -> {
                                                        if (!attached() || generation != viewGeneration
                                                                || !imported.isEnabled()) return true;
                                                        final boolean chosen = entry.chosenHomePlmns != null
                                                                && Arrays.asList(entry.chosenHomePlmns).contains(home);
                                                        importDialog = new AlertDialog.Builder(requireContext())
                                                                .setTitle(entry.sourceName)
                                                                .setMessage(item.getSummary())
                                                                .setNegativeButton(
                                                                        android.R.string.cancel,
                                                                        null)
                                                                .setPositiveButton(
                                                                        chosen ? R.string.use_bundled_profile : R.string.apply_import,
                                                                        (d, which) -> work(() -> {
                                                                            String current = context.getSystemService(TelephonyManager.class)
                                                                                    .createForSubscriptionId(selected).getSimOperator();
                                                                            if (!active(context, selected) || !home.equals(current))
                                                                                throw new IllegalStateException(context.getString(R.string.sim_changed));
                                                                            worker().chooseImportedProfile(home, chosen ? "" : entry.id);
                                                                            if (!chosen)
                                                                                prefs.edit().putBoolean("automatic", true)
                                                                                        .putBoolean("disable_requested", false).commit();
                                                                            // The choice is durable in the loader; acknowledge its explicit trigger.
                                                                            McfgCoordinatorService.requestConfiguration(prefs);
                                                                        }))
                                                                .setNeutralButton(
                                                                        R.string.delete_import,
                                                                        (d, which) ->
                                                                                work(
                                                                                        () ->
                                                                                                worker().deleteImportedProfile(
                                                                                                                entry.id)))
                                                                .show();
                                                        return true;
                                                    });
                                            imported.addPreference(item);
                                            importRows.put(item, entry);
                                        }
                                    updateImportActions();
                                });
                    } catch (Exception ignored) {
                    }
                });
    }

    @Override
    public void onResume() {
        super.onResume();
        resumed = true;
        if (!attached()) return;
        requireActivity().setTitle(R.string.app_name);
        if (!listening) {
            subscriptions.addOnSubscriptionsChangedListener(appContext.getMainExecutor(), subListener);
            listening = true;
        }
        updateSubscriptions();
        main.post(refresh);
        reloadImports();
    }

    @Override
    public void onPause() {
        resumed = false;
        main.removeCallbacks(refresh);
        unregisterSubscriptions();
        super.onPause();
    }

    private void unregisterSubscriptions() {
        if (!listening) return;
        subscriptions.removeOnSubscriptionsChangedListener(subListener);
        listening = false;
    }

    @Override
    public void onDestroyView() {
        resumed = false;
        preferencesAlive = false;
        ++viewGeneration;
        unregisterSubscriptions();
        // Guarded worker completions still clear working while this fragment is
        // on the back stack. Dropping them could leave imports disabled forever.
        main.removeCallbacks(refresh);
        if (importDialog != null) {
            importDialog.dismiss();
            importDialog = null;
        }
        super.onDestroyView();
    }

    @Override
    public void onDestroy() {
        unregisterSubscriptions();
        io.shutdown();
        poller.shutdown();
        main.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
