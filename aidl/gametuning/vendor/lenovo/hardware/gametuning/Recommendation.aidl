// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.PolicyProfile;

@VintfStability
parcelable Recommendation {
    // Stock key or "default"; empty for a user tuning.
    String profileKey;
    int baseMode;
    // Non-empty for a user tuning of another game.
    String tunedPackage;
    int tunedMode;
    float score;
    int reasons;
    // Gold, prime, gpu: -1 lower, 0 same, 1 higher than the record.
    int[] directions;
    PolicyProfile profile;
    long rangeRevision;
    int diagnosis;
    int ceilingFps;
    boolean confirmedCap;
    int analysisVersion;
}
