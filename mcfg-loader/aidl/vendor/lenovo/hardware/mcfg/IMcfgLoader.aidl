// SPDX-License-Identifier: Apache-2.0
package vendor.lenovo.hardware.mcfg;
import vendor.lenovo.hardware.mcfg.IMcfgCallback;
import vendor.lenovo.hardware.mcfg.McfgStatus;
import vendor.lenovo.hardware.mcfg.ProfileEntry;
import android.os.ParcelFileDescriptor;

@VintfStability
interface IMcfgLoader {
    McfgStatus getStatus();
    ProfileEntry[] getProfiles();
    void updateContext(in String[] homePlmns, String baseband, boolean safe);
    McfgStatus setEnabled(boolean enabled);
    McfgStatus apply(String transactionToken);
    McfgStatus restore(String transactionToken);
    boolean claimRestart(String transactionToken);
    McfgStatus complete(String transactionToken);
    McfgStatus probeReadOnly();
    void setCallback(IMcfgCallback callback);
    String importProfile(in ParcelFileDescriptor mbn, String filename, String homePlmn);
    void deleteImportedProfile(String id);
}
