// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.PolicyProfile;
import vendor.lenovo.hardware.gametuning.RecordingSummary;

@VintfStability
parcelable RecordingInfo {
    String id;
    int userId;
    String packageName;
    int mode;
    long startEpochMs;
    int durationMs;
    int sampleCount;
    int endReason;
    PolicyProfile profile;
    RecordingSummary summary;
    int originalDurationMs;
    int originalSampleCount;
    int keepStartMs;
    int keepEndMs;
    long rangeRevision;
}
