// SPDX-License-Identifier: Apache-2.0
// Reconstructed declaration of an existing vendor interface; used only for client headers.
package vendor.qti.qspmhal;
import vendor.qti.qspmhal.ProfData;
import vendor.qti.qspmhal.ParcelableMemory;
import vendor.qti.qspmhal.Command;
@VintfStability
interface IQspmhal {
    ProfData getGpuProf(long pid);
    ProfData getCpuProf(String name);
    void setAppInfoH(long pid, String packageName, String packageVersion);
    void setAppProfile(in ParcelableMemory memory, long size, String name, Command command);
}
