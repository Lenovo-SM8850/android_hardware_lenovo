// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable ProfileSource {
    // First stock package of the entry, or "default".
    String profileKey;
    // Stock entry name; empty for "default".
    String displayName;
    // Stock packages plus overlay aliases resolving to this entry.
    String[] packages;
    int[] baseModes;
}
