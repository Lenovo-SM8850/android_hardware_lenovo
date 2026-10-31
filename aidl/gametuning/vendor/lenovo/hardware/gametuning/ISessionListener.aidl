// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.gametuning;

@VintfStability
interface ISessionListener {
    oneway void onSessionChanged(int userId, String packageName, int mode, boolean active, boolean recording);
}
