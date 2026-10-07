// SPDX-License-Identifier: Apache-2.0
package vendor.lenovo.hardware.mcfg;
@VintfStability
parcelable ProfileEntry {
    String id;
    String[] homePlmns;
    String archiveSha256;
    int entryCount;
    boolean imported = false;
    String sourceName = "";
    String sourceSha256 = "";
}
