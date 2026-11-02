// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.GameTuningConfig;
import vendor.lenovo.hardware.gametuning.PolicyProfile;
import vendor.lenovo.hardware.gametuning.TuningChange;

@VintfStability
parcelable TuningProposal {
    GameTuningConfig tuning;
    TuningChange[] changes;
    int reasons;
    PolicyProfile preview;
    // The single change of this round: resource 0 gold, 1 prime, 2 gpu; -1 is the thermal case.
    int probeResource = -1;
    int probeDeltaSteps;
    long rangeRevision;
    int diagnosis;
    int ceilingFps;
    boolean confirmedCap;
    int analysisVersion;
}
