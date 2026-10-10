// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.telephony.SubscriptionManager;
import android.telephony.ims.ProvisioningManager;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.EditTextPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceGroup;

import com.android.settingslib.widget.AppSwitchPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

/** Carrier configuration overrides for the SIM selected on the main screen. */
public final class CarrierOverridesFragment extends SettingsBasePreferenceFragment {
    private int subId;
    private String expertKey = "";
    private PreferenceGroup carrier;
    private CarrierOverrides overrides;
    private Preference reset;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    followSubscription();
                }
            };
    private final Runnable refresh =
            new Runnable() {
                @Override
                public void run() {
                    carrier.setEnabled(CarrierSettingsFragment.active(requireContext(), subId));
                    main.postDelayed(this, 1000);
                }
            };

    private boolean active() {
        return CarrierSettingsFragment.active(requireContext(), subId);
    }

    @Override
    public void onCreatePreferences(Bundle saved, String rootKey) {
        getPreferenceManager().setStorageDeviceProtected();
        subId = getArguments().getInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, -1);
        if (saved != null) {
            subId = saved.getInt("selected_sub", subId);
            expertKey = saved.getString("expert_key", "");
        }
        setPreferenceScreen(getPreferenceManager().createPreferenceScreen(requireContext()));
        carrier = getPreferenceScreen();
        rebuildCarrier();
    }

    @Override
    public void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("selected_sub", subId);
        out.putString("expert_key", expertKey);
    }

    // Follow the same SIM fallback as the main screen so a transient SIM reload
    // does not strand this screen on a subscription that no longer exists.
    private void followSubscription() {
        int next = CarrierSettingsFragment.resolveSubscription(requireContext(), subId);
        if (next == subId) return;
        subId = next;
        getArguments().putInt(SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX, subId);
        rebuildCarrier();
    }

    private void error(String text) {
        if (isAdded())
            new AlertDialog.Builder(requireContext())
                    .setTitle(R.string.operation_failed)
                    .setMessage(text)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
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
        requireActivity().setTitle(R.string.carrier_overrides);
        requireContext()
                .getSystemService(SubscriptionManager.class)
                .addOnSubscriptionsChangedListener(requireContext().getMainExecutor(), subListener);
        followSubscription();
        main.post(refresh);
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
        main.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
