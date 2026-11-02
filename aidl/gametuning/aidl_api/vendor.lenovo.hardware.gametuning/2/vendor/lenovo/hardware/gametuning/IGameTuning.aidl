// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////
// THIS FILE IS IMMUTABLE. DO NOT EDIT IN ANY CASE.                          //
///////////////////////////////////////////////////////////////////////////////

// This file is a snapshot of an AIDL file. Do not edit it manually. There are
// two cases:
// 1). this is a frozen version file - do not edit this in any case.
// 2). this is a 'current' file. If you make a backwards compatible change to
//     the interface (from the latest frozen version), the build system will
//     prompt you to update this file with `m <name>-update-api`.
//
// You must not make a backward incompatible change to any AIDL file built
// with the aidl_interface module type with versions property set. The module
// type is used to build AIDL files in a way that they can be used across
// independently updatable components of the system. If a device is shipped
// with such a backward incompatible change, it has a high risk of breaking
// later when a module using the interface is updated, e.g., Mainline modules.

package vendor.lenovo.hardware.gametuning;
@VintfStability
interface IGameTuning {
  vendor.lenovo.hardware.gametuning.PolicyLimits getLimits();
  vendor.lenovo.hardware.gametuning.ProfileSource[] getProfileSources();
  vendor.lenovo.hardware.gametuning.PolicyProfile getProfile(int userId, String packageName, int mode, boolean tuned);
  vendor.lenovo.hardware.gametuning.PolicyProfile getSourceProfile(String profileKey, int baseMode);
  vendor.lenovo.hardware.gametuning.GameTuningConfig[] getTunings(int userId);
  vendor.lenovo.hardware.gametuning.PolicyProfile previewTuning(in vendor.lenovo.hardware.gametuning.GameTuningConfig tuning);
  vendor.lenovo.hardware.gametuning.GameTuningConfig setTuning(in vendor.lenovo.hardware.gametuning.GameTuningConfig tuning);
  void clearTuning(int userId, String packageName, int mode);
  void setRecordingArmed(int userId, String packageName, boolean armed);
  String[] getArmedPackages(int userId);
  vendor.lenovo.hardware.gametuning.RecordingInfo[] getRecordings(int userId, String packageName);
  vendor.lenovo.hardware.gametuning.RecordingSample[] getRecordingSamples(int userId, String id, int offset);
  void deleteRecording(int userId, String id);
  vendor.lenovo.hardware.gametuning.Recommendation[] recommend(int userId, String id, int goal);
  vendor.lenovo.hardware.gametuning.TuningProposal autoTune(int userId, String id, int goal);
  void registerSessionListener(in vendor.lenovo.hardware.gametuning.ISessionListener listener);
  void unregisterSessionListener(in vendor.lenovo.hardware.gametuning.ISessionListener listener);
  vendor.lenovo.hardware.gametuning.RecordingInfo setRecordingRange(int userId, String id, int startMs, int endMs, long expectedRevision);
  vendor.lenovo.hardware.gametuning.RecordingSample[] getEffectiveRecordingSamples(int userId, String id, int offset, long expectedRevision);
  vendor.lenovo.hardware.gametuning.RecordingSample[] getRawRecordingSamples(int userId, String id, int offset, long expectedRevision);
  void setRecordingContext(int userId, String packageName, int desiredFps, float renderScale, float refreshRate);
  void invalidateDisplayProbe(int userId, String packageName, int mode);
  boolean getGraphicsSupport();
  vendor.lenovo.hardware.gametuning.GraphicsOptions getGraphicsOptions(String packageName);
  void setGraphicsOptions(String packageName, in vendor.lenovo.hardware.gametuning.GraphicsOptions options);
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
  const int TEMPERATURE_UNAVAILABLE = (-273000) /* -273000 */;
  const int DIAGNOSIS_UNCLEAR = 0;
  const int DIAGNOSIS_GAME_CAP = 1;
  const int DIAGNOSIS_LIKELY_GAME_CAP = 2;
  const int DIAGNOSIS_FPS_LIMITED = 3;
  const int DIAGNOSIS_PERFORMANCE_LIMITED = 4;
  const int DIAGNOSIS_THERMAL_LIMITED = 5;
  const int DIAGNOSIS_MIXED_LIMITS = 6;
}
