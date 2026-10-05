// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.lightring;

import vendor.lenovo.hardware.lightring.Effect;

@VintfStability
interface ILightRing {
    const int STATIC = 0;
    const int BREATH = 1;
    const int RAINBOW = 2;
    const int COLOR_CYCLE = 3;
    void setEffect(in Effect effect);
    void clearEffect();
}
