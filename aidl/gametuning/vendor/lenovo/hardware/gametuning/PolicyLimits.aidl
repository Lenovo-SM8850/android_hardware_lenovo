// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable PolicyLimits {
    // Ascending runtime OPPs at or below the hard ceiling.
    int[] goldKHz;
    int[] primeKHz;
    int[] gpuHz;
    int goldCores;
    int primeCores;
    // 100, 200, 300, 400.
    int[] thermalCases;
}
