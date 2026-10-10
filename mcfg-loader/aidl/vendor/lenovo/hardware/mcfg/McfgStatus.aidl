// SPDX-License-Identifier: Apache-2.0
package vendor.lenovo.hardware.mcfg;
@VintfStability
parcelable McfgStatus {
    String state;
    String profileId;
    String candidateProfileId;
    String firmwareGeneration;
    String transactionToken;
    String detail;
    boolean enabled;
    boolean writesAllowed;
    boolean restartAttempted;
    String sourceName = "";
    String sourceSha256 = "";
    // -1 unavailable; 7 exactly 07; 0 absent, malformed, or nonautomatic.
    int autoselect = -1;
    String nextProfileId = "";
}
