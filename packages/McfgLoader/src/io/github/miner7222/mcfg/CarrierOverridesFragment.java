// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.telephony.SubscriptionManager;
import android.telephony.ims.ProvisioningManager;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.EditTextPreference;
import androidx.preference.EditTextPreferenceDialogFragmentCompat;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceGroup;

import com.android.settingslib.widget.AppSwitchPreference;
import com.android.settingslib.widget.FooterPreference;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import java.util.function.Consumer;

/** Carrier configuration overrides for the SIM selected on the main screen. */
public final class CarrierOverridesFragment extends SettingsBasePreferenceFragment {
    private static final String VALUE_DIALOG = "mcfg_value_dialog";
    // Input styles of the value editors.
    private static final int TEXT = 0, TECHNICAL = 1, NUMBER = 2;
    private int subId;
    private String expertKey = "";
    private PreferenceGroup carrier;
    // Group that receives the rows being added.
    private PreferenceGroup current;
    private Preference carrierName, expertValue;
    private CarrierOverrides overrides;
    private Preference reset;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final SubscriptionManager.OnSubscriptionsChangedListener subListener =
            new SubscriptionManager.OnSubscriptionsChangedListener() {
                @Override
                public void onSubscriptionsChanged() {
                    if (!isResumed() || getContext() == null) return;
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

    private PreferenceCategory group(int title) {
        PreferenceCategory category = new PreferenceCategory(requireContext());
        category.setTitle(title);
        carrier.addPreference(category);
        current = category;
        return category;
    }

    private Preference action(PreferenceGroup group, int title, int icon, Runnable run) {
        Preference p = new Preference(requireContext());
        p.setTitle(title);
        p.setIcon(icon);
        p.setOnPreferenceClickListener(
                preference -> {
                    run.run();
                    return true;
                });
        group.addPreference(p);
        return p;
    }

    private AppSwitchPreference bool(int title, String key) {
        return bool(title, key, null);
    }

    private AppSwitchPreference bool(int title, String key, Consumer<Boolean> changed) {
        AppSwitchPreference toggle = new AppSwitchPreference(requireContext());
        toggle.setTitle(title);
        toggle.setPersistent(false);
        toggle.setChecked(overrides.config().getBoolean(key));
        toggle.setOnPreferenceChangeListener(
                (p, value) -> {
                    try {
                        overrides.bool(key, (Boolean) value);
                        if (changed != null) changed.accept((Boolean) value);
                        return true;
                    } catch (Exception e) {
                        error(e.getMessage());
                        return false;
                    }
                });
        current.addPreference(toggle);
        return toggle;
    }

    private void rebuildCarrier() {
        carrier.removeAll();
        FooterPreference footer = new FooterPreference(requireContext());
        footer.setTitle(R.string.override_limits);
        carrier.addPreference(footer);
        carrier.setEnabled(active());
        if (!active()) return;
        overrides = new CarrierOverrides(requireContext(), subId);
        overrides.setOnChanged(
                () -> {
                    if (reset != null) reset.setEnabled(overrides.hasOverrides());
                });
        try {
            group(R.string.group_calls);
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
            group(R.string.group_network);
            bool(R.string.add_apns, "allow_adding_apns_bool");
            bool(R.string.ss_ut, "carrier_supports_ss_over_ut_bool");
            bool(R.string.ss_cdma, "support_ss_over_cdma_bool");
            bool(R.string.editable_lte, "editable_enhanced_4g_lte_bool");
            bool(R.string.default_lte, "enhanced_4g_lte_on_by_default_bool");
            bool(R.string.hide_lte, "hide_enhanced_4g_lte_bool");
            nr(R.string.nr_sa, 2);
            nr(R.string.nr_nsa, 1);
            group(R.string.group_display);
            bool(R.string.wfc_icon, "show_wifi_calling_icon_in_status_bar_bool");
            bool(R.string.ims_status, "show_ims_registration_status_bool");
            bool(R.string.four_g_icon, "show_4g_for_lte_data_icon_bool");
            bool(R.string.hide_plus, "hide_lte_plus_data_icon_bool");
            bool(R.string.always_icon, "always_show_data_rat_icon_bool");
            bool(R.string.inflate_signal, "inflate_signal_strength_bool");
            AppSwitchPreference nameOverride =
                    bool(
                            R.string.carrier_name_override,
                            "carrier_name_override_bool",
                            on -> carrierName.setEnabled(on));
            carrierName = editor(R.string.carrier_name, "carrier_name_string", TEXT);
            carrierName.setEnabled(nameOverride.isChecked());
            editor(R.string.five_g_icons, "5g_icon_configuration_string", TECHNICAL);
            editor(R.string.nr_thresholds, "5g_nr_ssrsrp_thresholds_int_array", TECHNICAL);
            editor(R.string.wfc_format, "wfc_spn_format_idx_int", NUMBER);
            editor(R.string.user_agent, "ims.ims_user_agent_string", TECHNICAL);
            group(R.string.group_custom);
            EditTextPreference key = new EditTextPreference(requireContext());
            key.setKey("expert_key");
            key.setTitle(R.string.expert_key);
            key.setDialogTitle(R.string.expert_key);
            key.setDialogLayoutResource(R.layout.dialog_edit_value);
            key.setOnBindEditTextListener(input(TECHNICAL));
            key.setPersistent(false);
            key.setText(expertKey);
            key.setSummary(expertKey.isEmpty() ? getString(R.string.not_set) : expertKey);
            key.setOnPreferenceChangeListener(
                    (p, value) -> {
                        expertKey = value.toString();
                        key.setSummary(
                                expertKey.isEmpty() ? getString(R.string.not_set) : expertKey);
                        expertValue.setEnabled(!expertKey.isEmpty());
                        return true;
                    });
            current.addPreference(key);
            expertValue = editor(R.string.expert_value, "", TECHNICAL);
            expertValue.setEnabled(!expertKey.isEmpty());
            group(R.string.group_actions);
            action(
                    current,
                    R.string.provision_volte,
                    R.drawable.ic_provision,
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
                    current,
                    R.string.restart_ims,
                    R.drawable.ic_restart_ims,
                    () -> {
                        try {
                            overrides.restartIms();
                        } catch (Exception e) {
                            error(e.getMessage());
                        }
                    });
            reset =
                    action(
                            current,
                            R.string.reset_carrier,
                            R.drawable.ic_reset,
                            () ->
                                    new AlertDialog.Builder(requireContext())
                                            .setTitle(R.string.reset_carrier)
                                            .setMessage(R.string.reset_carrier_summary)
                                            .setNegativeButton(android.R.string.cancel, null)
                                            .setPositiveButton(
                                                    R.string.reset_carrier_action,
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
        current.addPreference(toggle);
    }

    private EditTextPreference.OnBindEditTextListener input(int kind) {
        return edit -> {
            if (kind == NUMBER)
                edit.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED);
            else if (kind == TECHNICAL)
                // Plain ASCII keyboard without suggestions.
                edit.setInputType(
                        InputType.TYPE_CLASS_TEXT
                                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
            if (kind != TEXT) {
                edit.setTypeface(Typeface.MONOSPACE);
                edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            }
        };
    }

    private EditTextPreference editor(int title, String key, int kind) throws Exception {
        EditTextPreference field = new EditTextPreference(requireContext());
        field.setKey(key.isEmpty() ? "expert_value" : "carrier:" + key);
        field.setTitle(title);
        field.setDialogTitle(title);
        field.setDialogLayoutResource(R.layout.dialog_edit_value);
        field.setOnBindEditTextListener(input(kind));
        field.setPersistent(false);
        Object value = key.isEmpty() ? null : overrides.config().get(key);
        String text =
                value == null
                        ? ""
                        : value.getClass().isArray()
                                ? new org.json.JSONArray(value).toString()
                                : value.toString();
        field.setText(text);
        int empty = key.isEmpty() ? R.string.expert_summary : R.string.not_set;
        field.setSummary(text.isEmpty() ? getString(empty) : text);
        field.setOnPreferenceChangeListener(
                (p, next) -> {
                    try {
                        overrides.edit(key.isEmpty() ? expertKey : key, next.toString());
                        field.setSummary(
                                next.toString().isEmpty() ? getString(empty) : next.toString());
                        return true;
                    } catch (Exception e) {
                        error(e.getMessage());
                        return false;
                    }
                });
        current.addPreference(field);
        return field;
    }

    @Override
    public void onDisplayPreferenceDialog(Preference preference) {
        if (preference instanceof EditTextPreference) {
            if (getParentFragmentManager().findFragmentByTag(VALUE_DIALOG) != null) return;
            ValueDialog dialog = ValueDialog.create(preference.getKey());
            dialog.setTargetFragment(this, 0);
            dialog.show(getParentFragmentManager(), VALUE_DIALOG);
            return;
        }
        super.onDisplayPreferenceDialog(preference);
    }

    /** Value editor centered in the window insets, at most 480dp wide. */
    public static final class ValueDialog extends EditTextPreferenceDialogFragmentCompat {
        static ValueDialog create(String key) {
            ValueDialog dialog = new ValueDialog();
            Bundle args = new Bundle(1);
            args.putString(ARG_KEY, key);
            dialog.setArguments(args);
            return dialog;
        }

        @Override
        public void onStart() {
            super.onStart();
            Window window = getDialog() == null ? null : getDialog().getWindow();
            if (window == null) return;
            DisplayMetrics metrics = getResources().getDisplayMetrics();
            int width =
                    Math.min(
                            Math.round(480 * metrics.density),
                            metrics.widthPixels - Math.round(48 * metrics.density));
            window.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
            window.setGravity(Gravity.CENTER);
            // The base class replaces the whole input mode with state-only flags.
            window.setSoftInputMode(
                    WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                            | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        }
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
