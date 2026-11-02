// SPDX-License-Identifier: Apache-2.0
// Reconstructed declaration of an existing vendor interface; used only for client headers.
package vendor.qti.qspmhal;
import vendor.qti.qspmhal.ParcelableMemory;
@VintfStability
parcelable ProfData {
    ParcelableMemory memory;
    int size;
    String packageName;
    @nullable String packageVersion;
    String modifiedTime;
}
