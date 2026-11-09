// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.touchpolicy;

@VintfStability
oneway interface ITouchPolicyCallback {
    void onPenStateChanged(int state);
}
