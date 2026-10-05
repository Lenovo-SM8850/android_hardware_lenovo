// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.lightring;

@VintfStability
parcelable Effect {
    int type;
    int[] colors;
    // 0 = slow, 1 = normal, 2 = fast.
    int speed;
    // RGB scaling, 0-255; does not change the current cap.
    int brightness;
}
