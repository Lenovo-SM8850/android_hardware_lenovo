// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import java.util.Set;

/** Loaded SIM identity epochs, independent of Android Binder. */
final class CoordinatorState {
    private String identity = "";
    private boolean loaded, simPresent;
    private boolean radiosReady;
    private long epoch;
    private long retryEpoch = -1, retryVersion = -1, retryAt;
    private int failures;
    private static final long[] RETRY_DELAYS = {30_000, 120_000, 600_000};
    // Home PLMNs in the stock wuji named commercial MBNs' MCFG_TRL.mnoid lists.
    private static final Set<String> BUILT_IN_PLMNS = Set.of(
            "45502", "45507", "46000", "46001", "46002", "46003", "46004", "46005",
            "46006", "46007", "46008", "46009", "46011", "46012", "46013", "46015",
            "46030", "46059");

    synchronized boolean observe(String value) {
        loaded = true;
        simPresent = !"no_sim".equals(value);
        // Removal preserves the last loaded identity/epoch, including its verified apply.
        if (!simPresent) return false;
        if (value.equals(identity)) return false;
        identity = value;
        ++epoch;
        return true;
    }

    synchronized void clear() {
        // Temporary SIM unloading during modem recovery is not an identity change.
        loaded = false;
    }

    synchronized void setRadiosReady(boolean ready) {
        radiosReady = ready;
    }

    synchronized String identity() {
        return identity;
    }

    synchronized boolean canComplete(boolean sawRadioDown, String restartIdentity) {
        return canCompleteIdentity(sawRadioDown, restartIdentity, false);
    }

    synchronized boolean canComplete(boolean sawRadioDown, String restartIdentity,
            boolean controlOnly, String transactionToken, String reservedToken) {
        return !transactionToken.isEmpty() && transactionToken.equals(reservedToken)
                && canCompleteIdentity(sawRadioDown, restartIdentity, controlOnly);
    }

    private boolean canCompleteIdentity(boolean sawRadioDown, String restartIdentity,
            boolean controlOnly) {
        return sawRadioDown && radiosReady && loaded
                && (controlOnly || (simPresent && !identity.isEmpty() && identity.equals(restartIdentity)));
    }

    static boolean observeRadio(boolean radioOn, String restartToken, boolean sawRadioDown) {
        return sawRadioDown || (!radioOn && !restartToken.isEmpty());
    }

    static boolean shouldRecover(String transactionState, boolean safeContext, boolean writesAllowed) {
        return safeContext && writesAllowed
                && ("staged".equals(transactionState) || "applying".equals(transactionState)
                        || "readback_verified".equals(transactionState)
                        || "rechecking".equals(transactionState)
                        || "rolling_back".equals(transactionState));
    }

    synchronized long epoch() {
        return epoch;
    }

    static boolean rebooted(String reservedBootId, String currentBootId) {
        return !reservedBootId.isEmpty()
                && !currentBootId.isEmpty()
                && !reservedBootId.equals(currentBootId);
    }

    static boolean shouldRestoreUnsupported(String selectionState, int loadedSimCount) {
        return "unsupported".equals(selectionState) && loadedSimCount == 1;
    }

    static boolean shouldNotifyUnsupported(boolean automatic, String identity, String lastNotified) {
        String operator = identity.split(":", -1)[0];
        return !automatic && operator.matches("[0-9]{5,6}")
                && !BUILT_IN_PLMNS.contains(operator) && !identity.equals(lastNotified);
    }

    static boolean shouldResetCarrierNotice(boolean automatic, String identity, String observed) {
        return automatic || !identity.equals(observed);
    }

    static boolean shouldRestoreAutomatic(String selectionState, int loadedSimCount,
            boolean hasProfile, int autoselect) {
        if (shouldRestoreUnsupported(selectionState, loadedSimCount))
            return hasProfile || autoselect == 0;
        return false;  // No SIM and mixed/unknown subscriptions preserve EFS and progress.
    }

    static boolean shouldCoordinate(boolean automatic, int loadedSimCount) {
        // Explicit OFF remains available without a SIM; automatic work pauses.
        return !automatic || loadedSimCount > 0;
    }

    static boolean needsApply(long verifiedEpoch, long verifiedVersion, long epoch, long version) {
        return verifiedEpoch != epoch || verifiedVersion != version;
    }

    static boolean shouldRecordApplied(String profileId) {
        // An empty profile is only the 07/reset phase, not the selected overlay.
        return !profileId.isEmpty();
    }

    static boolean shouldRecordApplied(String transactionState, String profileId, String sourceName) {
        // "committed" is the loader's durable applied state, after the restart.
        return "committed".equals(transactionState)
                && shouldRecordApplied(profileId) && !sourceName.isEmpty();
    }

    static String displayState(String transactionState, String profileId, String sourceName,
            String candidateProfileId) {
        return displayState(transactionState, profileId, sourceName, candidateProfileId, true, true);
    }

    static String displayState(String transactionState, String profileId, String sourceName,
            String candidateProfileId, boolean automatic, boolean loaderEnabled) {
        // OFF immediately hides the previous overlay, including an old apply
        // error. Once the loader disables it, an error belongs to the restore.
        if (!automatic && loaderEnabled) return "default";
        if ("error".equals(transactionState)) return "error";
        if (!automatic || !loaderEnabled) return "default";
        if ("baseline_restart_pending".equals(transactionState)) return "baseline";
        if (candidateProfileId.isEmpty()) return "waiting";
        return shouldRecordApplied(transactionState, profileId, sourceName)
                        && candidateProfileId.equals(profileId)
                ? "applied" : "default";
    }

    static boolean needsApply(long verifiedEpoch, long verifiedVersion, long epoch, long version,
            String transactionState, String profileId, String sourceName, String candidateProfileId) {
        // The committed journal takes precedence over the process-local SIM epoch.
        // The acknowledged configuration version is persisted by the coordinator.
        return verifiedVersion != version
                || !shouldRecordApplied(transactionState, profileId, sourceName)
                || !candidateProfileId.equals(profileId);
    }

    static boolean isCurrentSubscription(int id, int slot, int mappedId, boolean present) {
        return id >= 0 && slot >= 0 && id == mappedId && present;
    }

    static boolean shouldMigrateAutomatic(boolean hasPreference, boolean loaderEnabled) {
        return !hasPreference && loaderEnabled;
    }

    static boolean shouldConfigureAutomatic(boolean enabled, boolean previousEnabled,
            boolean explicitRequest) {
        return enabled != previousEnabled || explicitRequest;
    }

    static boolean isRestartPending(String transactionState) {
        return "restart_pending".equals(transactionState)
                || "baseline_restart_pending".equals(transactionState);
    }

    static boolean shouldInitializeAutomatic(boolean loaderEnabled, long version, String transactionState,
            String profileId) {
        return (!loaderEnabled || version > 0)
                && !(isRestartPending(transactionState) && profileId.isEmpty());
    }

    synchronized boolean retryAllowed(long attemptedEpoch, long version, long now) {
        if (attemptedEpoch != epoch) return false;
        if (retryEpoch != attemptedEpoch || retryVersion != version) {
            retryEpoch = attemptedEpoch;
            retryVersion = version;
            failures = 0;
            retryAt = 0;
        }
        return now >= retryAt;
    }

    synchronized void attemptFailed(long attemptedEpoch, long version, long now) {
        // Ignore a failure from work superseded by a SIM change.
        if (attemptedEpoch != epoch) return;
        retryAllowed(attemptedEpoch, version, now);
        retryAt = now + RETRY_DELAYS[Math.min(failures, RETRY_DELAYS.length - 1)];
        if (failures < RETRY_DELAYS.length) ++failures;
    }
}
