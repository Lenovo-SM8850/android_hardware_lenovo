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
parcelable GameTuningConfig {
  int userId;
  String packageName;
  int mode;
  boolean enabled;
  String baseProfile;
  int baseMode;
  int[] maxOffsets;
  int[] minOffsets;
  vendor.lenovo.hardware.gametuning.PolicyStep[] steps;
  int thermalCase = KEEP /* -2 */;
  int uclampMin = KEEP /* -2 */;
  int gpuDefaultHz = KEEP /* -2 */;
  int keepCoresOnline = KEEP /* -2 */;
  int frameBoost = KEEP /* -2 */;
  int[] frameBoostSteps;
  int jankBoost = KEEP /* -2 */;
  int[] jankBoostSteps;
  int startDelayMs = KEEP /* -2 */;
  int threadRules = 1;
  int sceneMode = KEEP /* -2 */;
  vendor.lenovo.hardware.gametuning.SceneRow[] scenes;
  vendor.lenovo.hardware.gametuning.ThreadRule[] customThreads;
  int boostLengthMs = KEEP /* -2 */;
  int dropSensitivity = KEEP /* -2 */;
  int hysteresisDeciC = KEEP /* -2 */;
  int skinTargetMilliC = KEEP /* -2 */;
  int powerTargetMw = KEEP /* -2 */;
  String variantId;
  String variantName;
  int dropLowerPermille = (-1) /* -1 */;
  int dropUpperPermille = (-1) /* -1 */;
  const int KEEP = (-2) /* -2 */;
}
