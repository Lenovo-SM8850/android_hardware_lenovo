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
parcelable PolicyProfile {
  String profileKey;
  int baseMode;
  boolean tuned;
  int thermalCase;
  vendor.lenovo.hardware.gametuning.PolicyStep[] steps;
  int uclampMin = (-1) /* -1 */;
  int gpuDefaultHz = (-1) /* -1 */;
  boolean keepCoresOnline;
  int[] frameBoostSteps;
  int[] jankBoostSteps;
  int startDelayMs;
  int threadRules;
  int threadRuleCount;
  int sceneCount;
  vendor.lenovo.hardware.gametuning.SceneRow[] scenes;
  int sceneMode;
  vendor.lenovo.hardware.gametuning.ThreadRule[] threads;
  int boostLengthMs;
  int dropSensitivity;
  int hysteresisDeciC;
  int skinTargetMilliC;
  int powerTargetMw;
  String variantId;
  String variantName;
  int dropLowerPermille = (-1) /* -1 */;
  int dropUpperPermille = (-1) /* -1 */;
}
