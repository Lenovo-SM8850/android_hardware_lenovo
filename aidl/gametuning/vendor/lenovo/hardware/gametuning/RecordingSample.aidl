// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
parcelable RecordingSample {
    int timeMs;
    // -1 when no frame report arrived in this second.
    float fps;
    float fpsMin;
    int jankFrames;
    int frames;
    int goldKHz;
    int primeKHz;
    int gpuHz;
    // Requested ceilings, -1 when none.
    int goldCapKHz;
    int primeCapKHz;
    int gpuCapHz;
    // IGameTuning.TEMPERATURE_UNAVAILABLE when unreadable.
    int skinMilliC;
    int cpuMilliC;
    int gpuMilliC;
    // -1 while charging or unreadable.
    int powerMw;
    int band;
    int thermalCase;
    int flags;
    int mode;
    int profileGeneration;
    int sceneId = -1;
    // Longest present-to-present interval in the second, -1 unknown.
    int maxFrameTimeUs = -1;
    // Gold, prime, 0 to 1000.
    int[] cpuUtilPermille;
    int gpuBusyPercent = -1;
    // IGameTuning.TEMPERATURE_UNAVAILABLE when unreadable.
    int batteryMilliC = -273000;
    int batteryPercent = -1;
    // Resolved target frame rate of this sample, -1 when unknown.
    float targetFps = -1.0f;
    float requestedFps = -1.0f;
    float throttleFps = -1.0f;
    int desiredTargetFps = -1;
    float renderScale = -1.0f;
    float refreshRate = -1.0f;
}
