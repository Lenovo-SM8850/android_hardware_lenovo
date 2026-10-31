// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.FrequencyRange;

@VintfStability
parcelable SceneRow {
    // 0 to 100000.
    int sceneId;
    boolean enabled = true;
    // 100 to 10000 in steps of 100.
    int holdMs;
    FrequencyRange gold;
    FrequencyRange prime;
    FrequencyRange gpu;
}
