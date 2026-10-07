// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.app.Activity;
import android.content.Intent;
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
import android.telephony.ims.ProvisioningManager;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.EditTextPreference;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceGroup;

import com.android.settingslib.widget.AppSwitchPreference;
import com.android.settingslib.widget.MainSwitchPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import vendor.lenovo.hardware.mcfg.IMcfgLoader;
import vendor.lenovo.hardware.mcfg.ProfileEntry;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CarrierSettingsFragment extends SettingsBasePreferenceFragment {
    private static final int PICK_MBN = 1;
    private int subId, importSubId;
    private String importHome = "", expertKey = "";
    private SharedPreferences prefs;
    private Preference status;
    private PreferenceCategory carrier, imported;
    private MainSwitchPreference automatic;
    private ListPreference sim;
    private CarrierOverrides overrides;
    private Preference reset;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    updateSubscriptions();
                }
            };
    private final Runnable refresh =
            new Runnable() {
                @Override
                public void run() {
                    status.setSummary(prefs.getString("status", getString(R.string.waiting)));
                    automatic.setChecked(prefs.getBoolean("automatic", true));
                    carrier.setEnabled(active());
                    imported.setEnabled(active());
                    main.postDelayed(this, 1000);
                }
            };

    private boolean active() {
        return UserHandle.myUserId() == UserHandle.USER_SYSTEM
                && requireContext()
                        .getSystemService(SubscriptionManager.class)
                        .isActiveSubscriptionId(subId);
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
            expertKey = saved.getString("expert_key", "");
        }
        setPreferenceScreen(getPreferenceManager().createPreferenceScreen(requireContext()));
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
                    rebuildCarrier();
                    reloadImports();
                    return true;
                });
        getPreferenceScreen().addPreference(sim);
        automatic = new MainSwitchPreference(requireContext());
        automatic.setTitle(R.string.automatic);
        automatic.setPersistent(false);
        automatic.setChecked(prefs.getBoolean("automatic", true));
        automatic.setEnabled(UserHandle.myUserId() == UserHandle.USER_SYSTEM);
        automatic.setOnPreferenceChangeListener(
                (p, value) -> {
                    boolean enabled = (Boolean) value;
                    prefs.edit()
                            .putBoolean("automatic", enabled)
                            .putBoolean("disable_requested", !enabled)
                            .commit();
                    service(null);
                    return true;
                });
        getPreferenceScreen().addPreference(automatic);
        AppSwitchPreference restart = new AppSwitchPreference(requireContext());
        restart.setTitle(R.string.restart);
        restart.setPersistent(false);
        restart.setChecked(prefs.getBoolean("restart", true));
        restart.setEnabled(UserHandle.myUserId() == UserHandle.USER_SYSTEM);
        restart.setOnPreferenceChangeListener(
                (p, value) -> {
                    prefs.edit().putBoolean("restart", (Boolean) value).commit();
                    service(null);
                    return true;
                });
        getPreferenceScreen().addPreference(restart);
        status = action(getPreferenceScreen(), R.string.notice, () -> {});
        status.setSelectable(false);
        action(
                getPreferenceScreen(),
                R.string.read_probe,
                () -> service(McfgCoordinatorService.PROBE));
        action(
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
        imported = category(R.string.imported_profiles);
        carrier = category(R.string.carrier_overrides);
        rebuildCarrier();
        reloadImports();
    }

    private void updateSubscriptions() {
        java.util.List<SubscriptionInfo> list =
                requireContext()
                        .getSystemService(SubscriptionManager.class)
                        .getActiveSubscriptionInfoList();
        if (list == null) list = java.util.Collections.emptyList();
        String[] labels = new String[list.size()], ids = new String[list.size()];
        for (int i = 0; i < list.size(); i++) {
            labels[i] = list.get(i).getDisplayName().toString();
            ids[i] = Integer.toString(list.get(i).getSubscriptionId());
        }
        int previous = subId;
        if (!Arrays.asList(ids).contains(Integer.toString(subId))) {
            int defaultSub = SubscriptionManager.getDefaultDataSubscriptionId();
            subId =
                    Arrays.asList(ids).contains(Integer.toString(defaultSub))
                            ? defaultSub
                            : list.isEmpty()
                                    ? SubscriptionManager.INVALID_SUBSCRIPTION_ID
                                    : list.get(0).getSubscriptionId();
        }
        getArguments().putInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId);
        sim.setEntries(labels);
        sim.setEntryValues(ids);
        sim.setValue(Integer.toString(subId));
        sim.setEnabled(!list.isEmpty());
        if (carrier != null && previous != subId) {
            rebuildCarrier();
            reloadImports();
        }
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
        Intent intent = new Intent(requireContext(), McfgCoordinatorService.class);
        if (action != null) intent.setAction(action);
        requireContext().startService(intent);
    }

    private void error(String text) {
        if (isAdded())
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
        io.execute(
                () -> {
                    try {
                        operation.run();
                        main.post(
                                () -> {
                                    if (isAdded()) {
                                        service(null);
                                        reloadImports();
                                    }
                                });
                    } catch (Exception e) {
                        main.post(
                                () ->
                                        error(
                                                e.getMessage() == null
                                                        ? e.getClass().getSimpleName()
                                                        : e.getMessage()));
                    }
                });
    }

    @Override
    public void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("selected_sub", subId);
        out.putInt("import_sub", importSubId);
        out.putString("import_home", importHome);
        out.putString("expert_key", expertKey);
    }

    @Override
    public void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_MBN
                || result != Activity.RESULT_OK
                || data == null
                || data.getData() == null) return;
        final Uri uri = data.getData();
        final int selectedSub = importSubId;
        final String home = importHome;
        final android.content.Context context = requireContext().getApplicationContext();
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
        final int selected = subId;
        final android.content.Context context = requireContext().getApplicationContext();
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
                                    if (!isAdded() || selected != subId) return;
                                    imported.removeAll();
                                    for (ProfileEntry entry : entries)
                                        if (entry.imported
                                                && Arrays.asList(entry.homePlmns).contains(home)) {
                                            Preference item = new Preference(requireContext());
                                            item.setTitle(entry.sourceName);
                                            item.setSummary(entry.sourceSha256);
                                            item.setOnPreferenceClickListener(
                                                    p -> {
                                                        new AlertDialog.Builder(requireContext())
                                                                .setTitle(R.string.delete_import)
                                                                .setMessage(entry.sourceName)
                                                                .setNegativeButton(
                                                                        android.R.string.cancel,
                                                                        null)
                                                                .setPositiveButton(
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
                                        }
                                    if (imported.getPreferenceCount() == 0) {
                                        Preference empty = new Preference(requireContext());
                                        empty.setTitle(R.string.no_imports);
                                        empty.setSelectable(false);
                                        imported.addPreference(empty);
                                    }
                                });
                    } catch (Exception ignored) {
                    }
                });
    }

    private void bool(int title, String key) {
        AppSwitchPreference toggle = new AppSwitchPreference(requireContext());
        toggle.setTitle(title);
        toggle.setPersistent(false);
        toggle.setChecked(overrides.config().getBoolean(key));
        toggle.setOnPreferenceChangeListener(
                (p, value) -> {
                    try {
                        overrides.bool(key, (Boolean) value);
                        return true;
                    } catch (Exception e) {
                        error(e.getMessage());
                        return false;
                    }
                });
        carrier.addPreference(toggle);
    }

    private void rebuildCarrier() {
        carrier.removeAll();
        carrier.setEnabled(active());
        if (!active()) return;
        overrides = new CarrierOverrides(requireContext(), subId);
        overrides.setOnChanged(
                () -> {
                    if (reset != null) reset.setEnabled(overrides.hasOverrides());
                });
        try {
            bool(R.string.volte, "carrier_volte_available_bool");
            bool(R.string.vonr, "vonr_enabled_bool");
            bool(R.string.vonr_visible, "vonr_setting_visibility_bool");
            bool(R.string.wfc, "carrier_wfc_ims_available_bool");
            bool(R.string.video, "carrier_vt_available_bool");
            bool(R.string.cross_sim, "carrier_cross_sim_ims_available_bool");
            bool(R.string.cross_sim_data, "enable_cross_sim_calling_on_opportunistic_data_bool");
            bool(R.string.wfc_roaming, "carrier_default_wfc_ims_roaming_enabled_bool");
            bool(R.string.wfc_wifi_only, "carrier_wfc_supports_wifi_only_bool");
            bool(R.string.wfc_mode, "editable_wfc_mode_bool");
            bool(R.string.wfc_roaming_mode, "editable_wfc_roaming_mode_bool");
            bool(R.string.wfc_icon, "show_wifi_calling_icon_in_status_bar_bool");
            bool(R.string.ims_status, "show_ims_registration_status_bool");
            bool(R.string.add_apns, "allow_adding_apns_bool");
            bool(R.string.ss_ut, "carrier_supports_ss_over_ut_bool");
            bool(R.string.ss_cdma, "support_ss_over_cdma_bool");
            bool(R.string.four_g_icon, "show_4g_for_lte_data_icon_bool");
            bool(R.string.hide_plus, "hide_lte_plus_data_icon_bool");
            bool(R.string.always_icon, "always_show_data_rat_icon_bool");
            bool(R.string.inflate_signal, "inflate_signal_strength_bool");
            bool(R.string.editable_lte, "editable_enhanced_4g_lte_bool");
            bool(R.string.default_lte, "enhanced_4g_lte_on_by_default_bool");
            bool(R.string.hide_lte, "hide_enhanced_4g_lte_bool");
            nr(R.string.nr_sa, 2);
            nr(R.string.nr_nsa, 1);
            editor(R.string.carrier_name, "carrier_name_string");
            bool(R.string.carrier_name_override, "carrier_name_override_bool");
            editor(R.string.five_g_icons, "5g_icon_configuration_string");
            editor(R.string.nr_thresholds, "5g_nr_ssrsrp_thresholds_int_array");
            editor(R.string.wfc_format, "wfc_spn_format_idx_int");
            editor(R.string.user_agent, "ims.ims_user_agent_string");
            EditTextPreference key = new EditTextPreference(requireContext());
            key.setKey("expert_key");
            key.setTitle(R.string.expert_key);
            key.setPersistent(false);
            key.setText(expertKey);
            key.setSummary(expertKey);
            key.setOnPreferenceChangeListener(
                    (p, value) -> {
                        expertKey = value.toString();
                        key.setSummary(expertKey);
                        return true;
                    });
            carrier.addPreference(key);
            editor(R.string.expert_value, "");
            action(
                    carrier,
                    R.string.provision_volte,
                    () -> {
                        try {
                            int result =
                                    ProvisioningManager.createForSubscriptionId(subId)
                                            .setProvisioningIntValue(
                                                    ProvisioningManager
                                                            .KEY_VOLTE_PROVISIONING_STATUS,
                                                    1);
                            if (result != 0)
                                throw new IllegalStateException(
                                        getString(R.string.provision_failed, result));
                        } catch (Exception e) {
                            error(e.getMessage());
                        }
                    });
            action(
                    carrier,
                    R.string.restart_ims,
                    () -> {
                        try {
                            overrides.restartIms();
                        } catch (Exception e) {
                            error(e.getMessage());
                        }
                    });
            reset =
                    action(
                            carrier,
                            R.string.reset_carrier,
                            () ->
                                    new AlertDialog.Builder(requireContext())
                                            .setTitle(R.string.reset_carrier)
                                            .setMessage(R.string.reset_carrier_summary)
                                            .setNegativeButton(android.R.string.cancel, null)
                                            .setPositiveButton(
                                                    R.string.reset_carrier,
                                                    (d, which) -> {
                                                        try {
                                                            overrides.reset();
                                                            rebuildCarrier();
                                                        } catch (Exception e) {
                                                            error(e.getMessage());
                                                        }
                                                    })
                                            .show());
            reset.setEnabled(overrides.hasOverrides());
        } catch (Exception e) {
            error(e.getMessage());
        }
    }

    private void nr(int title, int type) {
        AppSwitchPreference toggle = new AppSwitchPreference(requireContext());
        toggle.setTitle(title);
        toggle.setPersistent(false);
        toggle.setChecked(overrides.nrEnabled(type));
        toggle.setOnPreferenceChangeListener(
                (p, value) -> {
                    try {
                        overrides.nr(
                                type == 2 ? (Boolean) value : overrides.nrEnabled(2),
                                type == 1 ? (Boolean) value : overrides.nrEnabled(1));
                        return true;
                    } catch (Exception e) {
                        error(e.getMessage());
                        return false;
                    }
                });
        carrier.addPreference(toggle);
    }

    private void editor(int title, String key) throws Exception {
        EditTextPreference field = new EditTextPreference(requireContext());
        field.setKey(key.isEmpty() ? "expert_value" : "carrier:" + key);
        field.setTitle(title);
        field.setPersistent(false);
        Object value = key.isEmpty() ? null : overrides.config().get(key);
        String text =
                value == null
                        ? ""
                        : value.getClass().isArray()
                                ? new org.json.JSONArray(value).toString()
                                : value.toString();
        field.setText(text);
        field.setSummary(key.isEmpty() ? getString(R.string.expert_summary) : text);
        field.setOnPreferenceChangeListener(
                (p, next) -> {
                    try {
                        overrides.edit(key.isEmpty() ? expertKey : key, next.toString());
                        field.setSummary(next.toString());
                        return true;
                    } catch (Exception e) {
                        error(e.getMessage());
                        return false;
                    }
                });
        carrier.addPreference(field);
    }

    @Override
    public void onResume() {
        super.onResume();
        requireContext()
                .getSystemService(SubscriptionManager.class)
                .addOnSubscriptionsChangedListener(requireContext().getMainExecutor(), subListener);
        updateSubscriptions();
        main.post(refresh);
        reloadImports();
    }

    @Override
    public void onPause() {
        main.removeCallbacks(refresh);
        requireContext()
                .getSystemService(SubscriptionManager.class)
                .removeOnSubscriptionsChangedListener(subListener);
        super.onPause();
    }

    @Override
    public void onDestroy() {
        io.shutdown();
        main.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
