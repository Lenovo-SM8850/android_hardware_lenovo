// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.PersistableBundle;
import android.telephony.CarrierConfigManager;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;

import org.json.JSONArray;

import java.util.HashSet;
import java.util.Set;

/** Independent platform API implementation; no GPL Pixel application code is reused. */
final class CarrierOverrides {
    static final String COMPONENT = "io.github.miner7222.mcfg/.McfgStatusActivity";
    private final Context context;
    private final int subId;
    private Runnable onChanged;

    CarrierOverrides(Context context, int subId) {
        this.context = context;
        this.subId = subId;
    }

    void setOnChanged(Runnable onChanged) {
        this.onChanged = onChanged;
    }

    // CarrierConfigManager cannot report overrides separately, so track the keys published here.
    private SharedPreferences tracked() {
        return context.createDeviceProtectedStorageContext()
                .getSharedPreferences("carrier_overrides", Context.MODE_PRIVATE);
    }

    boolean hasOverrides() {
        return !tracked().getStringSet(String.valueOf(subId), Set.of()).isEmpty();
    }

    private void changed() {
        if (onChanged != null) onChanged.run();
    }

    private CarrierConfigManager manager() {
        return context.getSystemService(CarrierConfigManager.class);
    }

    private void active() {
        if (!context.getSystemService(SubscriptionManager.class).isActiveSubscriptionId(subId))
            throw new IllegalStateException("Subscription is no longer active");
    }

    PersistableBundle config() {
        active();
        PersistableBundle result = manager().getConfigForSubId(subId);
        if (result == null) throw new IllegalStateException("Carrier configuration is unavailable");
        return result;
    }

    static void pinEntry(PersistableBundle values) {
        values.putBoolean(CarrierConfigManager.KEY_CARRIER_SETTINGS_ENABLE_BOOL, true);
        values.putString(
                CarrierConfigManager.KEY_CARRIER_SETTINGS_ACTIVITY_COMPONENT_NAME_STRING,
                COMPONENT);
    }

    void publish(PersistableBundle values) {
        active();
        pinEntry(values);
        // CarrierConfigLoader merges with existing overrides; platform signing permits persistence.
        manager().overrideConfig(subId, values, true);
        Set<String> keys = new HashSet<>(tracked().getStringSet(String.valueOf(subId), Set.of()));
        keys.addAll(values.keySet());
        keys.remove(CarrierConfigManager.KEY_CARRIER_SETTINGS_ENABLE_BOOL);
        keys.remove(CarrierConfigManager.KEY_CARRIER_SETTINGS_ACTIVITY_COMPONENT_NAME_STRING);
        tracked().edit().putStringSet(String.valueOf(subId), keys).apply();
        changed();
    }

    void reset() {
        active();
        manager().overrideConfig(subId, null, true);
        publish(new PersistableBundle());
        tracked().edit().remove(String.valueOf(subId)).apply();
        changed();
    }

    void bool(String key, boolean value) {
        PersistableBundle values = new PersistableBundle();
        values.putBoolean(key, value);
        publish(values);
    }

    void nr(boolean sa, boolean nsa) {
        PersistableBundle values = new PersistableBundle();
        values.putIntArray(
                CarrierConfigManager.KEY_CARRIER_NR_AVAILABILITIES_INT_ARRAY,
                sa && nsa
                        ? new int[] {1, 2}
                        : sa ? new int[] {2} : nsa ? new int[] {1} : new int[0]);
        publish(values);
    }

    boolean nrEnabled(int type) {
        int[] types =
                config().getIntArray(CarrierConfigManager.KEY_CARRIER_NR_AVAILABILITIES_INT_ARRAY);
        if (types != null) for (int item : types) if (item == type) return true;
        return false;
    }

    void edit(String key, String text) throws Exception {
        if (key.equals(CarrierConfigManager.KEY_CARRIER_SETTINGS_ENABLE_BOOL)
                || key.equals(
                        CarrierConfigManager.KEY_CARRIER_SETTINGS_ACTIVITY_COMPONENT_NAME_STRING))
            throw new IllegalArgumentException(context.getString(R.string.managed_entry));
        Object previous = config().get(key);
        PersistableBundle values = new PersistableBundle();
        if (previous instanceof Boolean) {
            if (!text.equals("true") && !text.equals("false"))
                throw new IllegalArgumentException(context.getString(R.string.boolean_required));
            values.putBoolean(key, Boolean.parseBoolean(text));
        } else if (previous instanceof Integer) values.putInt(key, Integer.parseInt(text));
        else if (previous instanceof Long) values.putLong(key, Long.parseLong(text));
        else if (previous instanceof Double) values.putDouble(key, Double.parseDouble(text));
        else if (previous instanceof String) values.putString(key, text);
        else {
            JSONArray array = new JSONArray(text);
            if (array.length() > 256)
                throw new IllegalArgumentException(context.getString(R.string.array_too_long));
            if (previous instanceof int[]) {
                int[] a = new int[array.length()];
                for (int i = 0; i < a.length; i++) a[i] = array.getInt(i);
                values.putIntArray(key, a);
            } else if (previous instanceof long[]) {
                long[] a = new long[array.length()];
                for (int i = 0; i < a.length; i++) a[i] = array.getLong(i);
                values.putLongArray(key, a);
            } else if (previous instanceof boolean[]) {
                boolean[] a = new boolean[array.length()];
                for (int i = 0; i < a.length; i++) a[i] = array.getBoolean(i);
                values.putBooleanArray(key, a);
            } else if (previous instanceof String[]) {
                String[] a = new String[array.length()];
                for (int i = 0; i < a.length; i++) a[i] = array.getString(i);
                values.putStringArray(key, a);
            } else if (previous instanceof double[]) {
                double[] a = new double[array.length()];
                for (int i = 0; i < a.length; i++) a[i] = array.getDouble(i);
                values.putDoubleArray(key, a);
            } else throw new IllegalArgumentException(context.getString(R.string.unknown_key));
        }
        publish(values);
    }

    void restartIms() {
        active();
        context.getSystemService(TelephonyManager.class)
                .resetIms(SubscriptionManager.getSlotIndex(subId));
    }
}
