// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.touchpolicy;

@VintfStability
parcelable TouchPolicyState {
    boolean sessionActive;
    int tapSensitivity;
    int moveSensitivity;
    int edgeRejection;
    boolean sessionPenInput;
    int penState;
    int supportPen;
}
