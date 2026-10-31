// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.FrequencyRange;

@VintfStability
parcelable PolicyStep {
    // Lower bound in degrees Celsius. The first step has no lower bound and stores 0.
    float temperatureC;
    FrequencyRange gold;
    FrequencyRange prime;
    FrequencyRange gpu;
}
