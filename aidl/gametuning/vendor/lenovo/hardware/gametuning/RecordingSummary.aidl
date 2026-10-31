// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable RecordingSummary {
    int validSamples;
    float avgFps;
    float medianFps;
    float lowFps1;
    float lowFps5;
    int targetFps;
    float hitRate;
    float stability;
    float jankPercent;
    float avgSkinC;
    float maxSkinC;
    float maxCpuC;
    float maxGpuC;
    int avgPowerMw;
    int avgGoldKHz;
    int avgPrimeKHz;
    int avgGpuHz;
    // Gold, prime, gpu shares 0..1.
    float[] pegged;
    float throttled;
    boolean mixed;
    float floor5Fps;
    float tail95;
    float jankPer1000;
    int[] scenesSeen;
}
