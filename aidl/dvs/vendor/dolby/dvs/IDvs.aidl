/*
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.dolby.dvs;

import vendor.dolby.dvs.IDvsCallback;

@VintfStability
interface IDvs {
    float[] getDolbyVisionStatus() = 0;
    void registerCallback(in IDvsCallback callback) = 1;
    void registerProcessor(in boolean enabled) = 2;
    void unregisterProcessor() = 3;
    void updateParams(in long paramId, in long value) = 4;
    void updateDisplayIDs(in int[] displayIDs) = 5;
    int[] getDisplayIDs() = 6;
    void setParameters(in String params) = 7;
    String getParameters(in String keys) = 8;
}
