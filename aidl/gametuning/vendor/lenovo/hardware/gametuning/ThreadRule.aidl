// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable ThreadRule {
    // 1 to 15 characters.
    String name;
    long cpuMask;
    // -1 is off, otherwise 90 to 99.
    int rtPriority = -1;
}
