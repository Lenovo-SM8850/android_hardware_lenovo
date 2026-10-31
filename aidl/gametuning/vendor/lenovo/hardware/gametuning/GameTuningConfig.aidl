// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.PolicyStep;
import vendor.lenovo.hardware.gametuning.SceneRow;
import vendor.lenovo.hardware.gametuning.ThreadRule;

@VintfStability
parcelable GameTuningConfig {
    const int KEEP = -2;
    int userId;
    String packageName;
    // GameManager mode: 1 standard, 2 performance, 3 battery.
    int mode;
    boolean enabled;
    // A stock profile key or "default". Empty selects the game's own profile.
    String baseProfile;
    int baseMode;
    // Gold, prime, gpu OPP steps.
    int[] maxOffsets;
    int[] minOffsets;
    // Empty uses the base profile's steps.
    PolicyStep[] steps;
    int thermalCase = KEEP;
    int uclampMin = KEEP;
    int gpuDefaultHz = KEEP;
    int keepCoresOnline = KEEP;
    int frameBoost = KEEP;
    int[] frameBoostSteps;
    int jankBoost = KEEP;
    int[] jankBoostSteps;
    int startDelayMs = KEEP;
    int threadRules = 1;
    // 0 off, 1 on (own stock rows, or scenes when non-empty).
    int sceneMode = KEEP;
    // Empty uses own stock rows. At most 32 rows.
    SceneRow[] scenes;
    // Used when threadRules is THREADS_CUSTOM. At most 16 rules, at most 8 with rtPriority 90 or higher.
    ThreadRule[] customThreads;
    // 100 to 5000 in steps of 100.
    int boostLengthMs = KEEP;
    // Legacy levels; new clients use the explicit window bounds.
    int dropSensitivity = KEEP;
    // 0 to 30, in tenths of a degree Celsius.
    int hysteresisDeciC = KEEP;
    // -1 off, otherwise 35000 to 48000 in steps of 500.
    int skinTargetMilliC = KEEP;
    // -1 off, otherwise 2000 to 12000 in steps of 500.
    int powerTargetMw = KEEP;
    // Opaque id of a saved variant, 0 to 64 characters of A-Z, a-z, 0-9, underscore and hyphen.
    String variantId;
    // Display name of the variant, 0 to 64 characters.
    String variantName;
    // Frame-drop trigger window in permille; both -1 use the stock window.
    int dropLowerPermille = -1;
    int dropUpperPermille = -1;
}
