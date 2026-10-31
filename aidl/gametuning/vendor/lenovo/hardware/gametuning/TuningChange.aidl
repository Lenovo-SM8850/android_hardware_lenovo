// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable TuningChange {
    int kind;
    int resource;
    int before;
    int after;
}
