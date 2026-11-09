// SPDX-License-Identifier: Apache-2.0

package vendor.lenovo.hardware.touchpolicy;

import vendor.lenovo.hardware.touchpolicy.ITouchPolicyCallback;
import vendor.lenovo.hardware.touchpolicy.TouchPolicyState;
import vendor.lenovo.hardware.touchpolicy.TouchSessionConfig;

@VintfStability
interface ITouchPolicy {
    const int PEN_DISCONNECTED = 0;
    const int PEN_CONNECTED = 1;
    const int PEN_LINKING = 2;

    // Starts or updates a game session and writes the session values.
    void setSession(in TouchSessionConfig config);
    // Restores the idle values and the pen connection state.
    void endSession();
    // Owns the pen node while a session is active; true keeps the connection state.
    void setSessionPenInput(boolean enabled);
    // The pen service reports 0 disconnected, 1 connected and 2 linking.
    void setPenState(int state);
    TouchPolicyState getState();
    void registerCallback(in ITouchPolicyCallback callback);
    void unregisterCallback(in ITouchPolicyCallback callback);
}
