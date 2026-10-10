// SPDX-License-Identifier: Apache-2.0
package vendor.lenovo.hardware.mcfg;
import vendor.lenovo.hardware.mcfg.McfgStatus;
@VintfStability
oneway interface IMcfgCallback {
    void onStatus(in McfgStatus status);
}
