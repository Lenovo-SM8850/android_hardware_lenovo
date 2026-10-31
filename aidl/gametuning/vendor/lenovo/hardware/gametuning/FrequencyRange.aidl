// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable FrequencyRange {
    // CPU values are kHz and GPU values are Hz. -1 sends no request for that bound.
    int min = -1;
    int max = -1;
}
