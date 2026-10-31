// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.PolicyStep;
import vendor.lenovo.hardware.gametuning.SceneRow;
import vendor.lenovo.hardware.gametuning.ThreadRule;

// Resolved values the service requests.
@VintfStability
parcelable PolicyProfile {
    String profileKey;
    int baseMode;
    boolean tuned;
    int thermalCase;
    PolicyStep[] steps;
    int uclampMin = -1;
    int gpuDefaultHz = -1;
    boolean keepCoresOnline;
    // Empty when off; otherwise gold, prime, gpu.
    int[] frameBoostSteps;
    // Empty when off.
    int[] jankBoostSteps;
    int startDelayMs;
    int threadRules;
    int threadRuleCount;
    int sceneCount;
    SceneRow[] scenes;
    int sceneMode;
    ThreadRule[] threads;
    int boostLengthMs;
    int dropSensitivity;
    int hysteresisDeciC;
    int skinTargetMilliC;
    int powerTargetMw;
    // The stored tuning's variant id and name; empty for a stock profile.
    String variantId;
    String variantName;
    // Frame-drop trigger window in permille; both -1 use the stock window.
    int dropLowerPermille = -1;
    int dropUpperPermille = -1;
}
