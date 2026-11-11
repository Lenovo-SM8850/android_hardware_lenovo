// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

import vendor.lenovo.hardware.gametuning.GameTuningConfig;
import vendor.lenovo.hardware.gametuning.GraphicsOptions;
import vendor.lenovo.hardware.gametuning.ISessionListener;
import vendor.lenovo.hardware.gametuning.PolicyLimits;
import vendor.lenovo.hardware.gametuning.PolicyProfile;
import vendor.lenovo.hardware.gametuning.ProfileSource;
import vendor.lenovo.hardware.gametuning.Recommendation;
import vendor.lenovo.hardware.gametuning.RecordingInfo;
import vendor.lenovo.hardware.gametuning.RecordingSample;
import vendor.lenovo.hardware.gametuning.TuningProposal;

@VintfStability
interface IGameTuning {
    const int BASE_BALANCED = 1;
    const int BASE_PERFORMANCE = 2;
    const int BASE_BATTERY = 3;
    const int BASE_MAXIMUM = 4;
    const int THREADS_OFF = 0;
    const int THREADS_OWN = 1;
    const int THREADS_BASE = 2;
    const int THREADS_CUSTOM = 3;
    const int GOAL_FRAMES = 1;
    const int GOAL_BALANCED = 2;
    const int GOAL_EFFICIENCY = 3;
    const int SENSITIVITY_LOW = 1;
    const int SENSITIVITY_MEDIUM = 2;
    const int SENSITIVITY_HIGH = 3;
    const int ERROR_NOT_FOUND = 1;
    const int ERROR_NOT_ENOUGH_DATA = 2;
    const int ERROR_MIXED = 3;
    const int ERROR_STORAGE = 4;
    const int ERROR_INVALID = 5;
    const int REASON_TARGET_MET = 1;
    const int REASON_FPS_SHORT = 2;
    const int REASON_CPU_BOUND = 4;
    const int REASON_GPU_BOUND = 8;
    const int REASON_THERMAL_LIMITED = 16;
    const int REASON_OVERPROVISIONED = 32;
    const int REASON_HOT = 64;
    const int REASON_NO_POWER = 128;
    const int FLAG_READY = 1;
    const int FLAG_SCENE = 2;
    const int FLAG_FPS_BOOST = 4;
    const int FLAG_JANK_BOOST = 8;
    const int FLAG_SENSOR_FAILED = 16;
    const int END_STOPPED = 1;
    const int END_EXITED = 2;
    const int END_LIMIT = 3;
    const int END_INTERRUPTED = 4;
    const int CHANGE_MAX_FREQ = 1;
    const int CHANGE_MIN_FREQ = 2;
    const int CHANGE_THERMAL_CASE = 3;
    const int CHANGE_FRAME_BOOST = 4;
    const int TEMPERATURE_UNAVAILABLE = -273000;

    PolicyLimits getLimits();
    ProfileSource[] getProfileSources();
    // The game's own resolution; tuned also applies its stored tuning.
    PolicyProfile getProfile(int userId, String packageName, int mode, boolean tuned);
    PolicyProfile getSourceProfile(String profileKey, int baseMode);
    GameTuningConfig[] getTunings(int userId);
    PolicyProfile previewTuning(in GameTuningConfig tuning);
    // Returns the normalized tuning that was stored and applies it to a matching session.
    GameTuningConfig setTuning(in GameTuningConfig tuning);
    void clearTuning(int userId, String packageName, int mode);
    void setRecordingArmed(int userId, String packageName, boolean armed);
    String[] getArmedPackages(int userId);
    // Empty packageName lists every record of the user, newest first.
    RecordingInfo[] getRecordings(int userId, String packageName);
    // At most 600 samples from offset.
    RecordingSample[] getRecordingSamples(int userId, String id, int offset);
    void deleteRecording(int userId, String id);
    Recommendation[] recommend(int userId, String id, int goal);
    TuningProposal autoTune(int userId, String id, int goal);
    void registerSessionListener(in ISessionListener listener);
    void unregisterSessionListener(in ISessionListener listener);
    // Inclusive original-time range. (0, originalDurationMs) resets to full.
    RecordingInfo setRecordingRange(int userId, String id, int startMs, int endMs, long expectedRevision);
    RecordingSample[] getEffectiveRecordingSamples(int userId, String id, int offset, long expectedRevision);
    RecordingSample[] getRawRecordingSamples(int userId, String id, int offset, long expectedRevision);
    // The app captures actual chosen display values before recording; unknown values stay -1.
    void setRecordingContext(int userId, String packageName, int desiredFps, float renderScale, float refreshRate);
    // Display changes invalidate single-resource probe attribution.
    void invalidateDisplayProbe(int userId, String packageName, int mode);
    const int DIAGNOSIS_UNCLEAR = 0;
    const int DIAGNOSIS_GAME_CAP = 1;
    const int DIAGNOSIS_LIKELY_GAME_CAP = 2;
    const int DIAGNOSIS_FPS_LIMITED = 3;
    const int DIAGNOSIS_PERFORMANCE_LIMITED = 4;
    const int DIAGNOSIS_THERMAL_LIMITED = 5;
    const int DIAGNOSIS_MIXED_LIMITS = 6;
    boolean getGraphicsSupport();
    GraphicsOptions getGraphicsOptions(String packageName);
    void setGraphicsOptions(String packageName, in GraphicsOptions options);
    const int GPP_SR = 32;
    const int GPP_FI = 64;
    // Runtime state only; never creates or changes a user tuning.
    void setGppState(int userId, String packageName, int flags);
}
