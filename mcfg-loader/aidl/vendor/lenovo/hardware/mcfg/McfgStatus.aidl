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
}
