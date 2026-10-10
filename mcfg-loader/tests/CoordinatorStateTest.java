// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

public final class CoordinatorStateTest {
    private static int checks;

    private static void check(boolean ok) {
        ++checks;
        if (!ok) throw new AssertionError("check " + checks);
    }

    private static void controlOnlyRestartChecks() {
        final String token = "control-only-token";
        for (String start : new String[] {"no_sim", "45006"}) {
            for (String recovered : new String[] {"no_sim", "45007"}) {
                CoordinatorState state = new CoordinatorState();
                state.setRadiosReady(true);
                state.observe(start);
                boolean sawDown = false; // Reservation resets the persisted witness.
                check(!state.canComplete(sawDown, start, true, token, token));
                sawDown = CoordinatorState.observeRadio(false, token, sawDown);
                state.setRadiosReady(false);
                // Radio-down arrives before the SIM LOADED state is cleared.
                check(!state.canComplete(sawDown, start, true, token, token));
                check(!state.canComplete(sawDown, start, false, token, token));
                state.clear(); // No fresh radio/SIM context during SSR.
                check(sawDown);
                check(!state.canComplete(sawDown, start, true, token, token));
                sawDown = CoordinatorState.observeRadio(true, token, sawDown);
                state.observe(recovered); // SIM identity may reload before radio-up.
                check(!state.canComplete(sawDown, start, true, token, token));
                state.setRadiosReady(true);
                check(state.canComplete(sawDown, start, true, token, token));
                check(!state.canComplete(sawDown, start, true, token, "stale-token"));
                check(!state.canComplete(sawDown, start, true, "", ""));
                if (!start.equals(recovered))
                    check(!state.canComplete(sawDown, start, false, token, token));
            }
        }
        CoordinatorState loaded = new CoordinatorState();
        loaded.setRadiosReady(true);
        loaded.observe("45006");
        boolean down = CoordinatorState.observeRadio(false, token, false);
        loaded.setRadiosReady(false);
        check(!loaded.canComplete(down, "45006", false, token, token));
        loaded.setRadiosReady(true); // Radio-up, SIM never reported unloaded.
        check(loaded.canComplete(down, "45006", false, token, token));
        check(!CoordinatorState.observeRadio(false, "", false));
        check(!CoordinatorState.observeRadio(true, token, false));
        for (String phase : new String[] {"staged", "applying", "readback_verified",
                "rechecking", "rolling_back"}) {
            for (String identity : new String[] {"no_sim", "45007"}) {
                CoordinatorState state = new CoordinatorState();
                state.observe(identity);
                check(CoordinatorState.shouldRecover(phase, true, true));
                check(!CoordinatorState.shouldRecover(phase, false, true));
                check(!CoordinatorState.shouldRecover(phase, true, false));
            }
        }
        for (String phase : new String[] {"restart_pending", "committed", "disabled", "error",
                "rollback_failed", "checking", "unsupported", "waiting_for_identity"})
            check(!CoordinatorState.shouldRecover(phase, true, true));
    }

    public static void main(String[] args) {
        controlOnlyRestartChecks();
        final String kt = "Korea/KT/Commercial_KT_LTE";
        final String source = "generic/Korea/KT/Commercial_KT_LTE/mcfg_sw.mbn";
        CoordinatorState retry = new CoordinatorState();
        retry.observe("45008");
        long retryEpoch = retry.epoch();
        for (String phase : new String[] {"waiting_for_identity", "checking", "staged", "applying",
                "readback_verified", "restart_pending", "error", "disabled", "unsupported"}) {
            check(CoordinatorState.needsApply(retryEpoch, 0, retryEpoch, 0,
                    phase, kt, source, kt)); // A stale verified epoch cannot suppress recovery.
            check(!CoordinatorState.shouldRecordApplied(phase, kt, source));
            check(CoordinatorState.displayState(phase, kt, source, kt)
                    .equals("error".equals(phase) ? "error" : "default"));
        }
        check(CoordinatorState.displayState("waiting_for_identity", kt, "", kt).equals("default"));
        check(CoordinatorState.displayState("error", kt, "", kt).equals("error"));
        check(CoordinatorState.displayState("committed", kt, "", kt).equals("default"));
        check(CoordinatorState.displayState("committed", "", source, kt).equals("default"));
        check(CoordinatorState.displayState("committed", kt, source, kt).equals("applied"));
        check(CoordinatorState.displayState("committed", kt, source, "").equals("waiting"));
        // OFF hides KT immediately while LGU is inserted, before setEnabled or
        // restore can reach the loader. A delayed status poll must not flash Applied.
        for (String phase : new String[] {"committed", "waiting_for_identity", "checking",
                "staged", "applying", "readback_verified", "restart_pending", "disabled"}) {
            for (boolean loaderEnabled : new boolean[] {true, false})
                check(CoordinatorState.displayState(phase, kt, source, kt, false, loaderEnabled)
                        .equals("default"));
        }
        check(CoordinatorState.displayState("error", kt, source, kt, false, true)
                .equals("default")); // Previous automatic apply failed, OFF not dispatched yet.
        check(CoordinatorState.displayState("error", kt, source, kt, false, false)
                .equals("error")); // Disabled loader's 07 restore actually failed.
        check(CoordinatorState.displayState("restart_pending", "", "", kt, false, false)
                .equals("default")); // 07 written and verified; waiting for its restart.
        check(CoordinatorState.displayState("disabled", "", "", kt, false, false)
                .equals("default")); // Successful OFF completion.
        check(CoordinatorState.displayState("committed", kt, source, kt, true, false)
                .equals("default"));
        check(CoordinatorState.displayState("error", kt, source, kt, true, true)
                .equals("error"));
        check(CoordinatorState.displayState("committed", kt, source, "Korea/LGU/Commercial")
                .equals("default"));
        check(!CoordinatorState.needsApply(retryEpoch, 0, retryEpoch, 0,
                "committed", kt, source, kt));
        // Process/reboot loses the verified epoch, but preserves the configuration version.
        check(!CoordinatorState.needsApply(-1, 0, 1, 0, "committed", kt, source, kt));
        check(!CoordinatorState.needsApply(8, 3, 1, 3, "committed", kt, source, kt));
        check(CoordinatorState.needsApply(-1, 3, 1, 3, "committed", kt, source,
                "Korea/LGU/Commercial")); // Changed SIM selects another profile.
        check(CoordinatorState.needsApply(-1, 3, 1, 4, "committed", kt, source, kt));
        check(CoordinatorState.needsApply(-1, 3, 1, 3, "committed", kt, source,
                "imported/chosen")); // Persisted imported choice changes the candidate.
        check(CoordinatorState.needsApply(-1, 3, 1, 3, "error", kt, source, kt));
        check(CoordinatorState.needsApply(-1, 3, 1, 3, "committed", kt, "", kt));
        retry.attemptFailed(retryEpoch, 0, 1000);
        check(!retry.retryAllowed(retryEpoch, 0, 30_999));
        check(retry.retryAllowed(retryEpoch, 0, 31_000));
        check(CoordinatorState.needsApply(retryEpoch, 0, retryEpoch, 0,
                "error", kt, "", kt)); // Pending profile retried after error, identity already loaded.
        check(CoordinatorState.needsApply(retryEpoch, 0, retryEpoch, 0,
                "waiting_for_identity", kt, "", kt)); // Old journal survives loader restart.
        check(!CoordinatorState.shouldInitializeAutomatic(true, 0, "error", kt));
        check(!CoordinatorState.shouldInitializeAutomatic(true, 0, "waiting_for_identity", kt));
        check(CoordinatorState.shouldInitializeAutomatic(true, 1, "error", kt)); // Explicit ON retry.
        check(CoordinatorState.shouldInitializeAutomatic(false, 0, "disabled", ""));
        check(!CoordinatorState.shouldInitializeAutomatic(false, 1, "restart_pending", ""));
        check(!CoordinatorState.isCurrentSubscription(1, 0, 3, true)); // Old LGU record.
        check(CoordinatorState.isCurrentSubscription(3, 0, 3, true)); // Current KT record.
        check(!CoordinatorState.isCurrentSubscription(2, -1, 2, true)); // Inactive/unknown record.
        check(!CoordinatorState.isCurrentSubscription(2, 1, 2, false)); // Empty slot.
        check(!CoordinatorState.isCurrentSubscription(-1, 0, -1, true));
        CoordinatorState removal = new CoordinatorState();
        removal.setRadiosReady(true);
        removal.observe("45006");
        long appliedEpoch = removal.epoch();
        check(!removal.observe("no_sim"));
        check(removal.epoch() == appliedEpoch && removal.identity().equals("45006"));
        check(!CoordinatorState.shouldCoordinate(true, 0)); // No reads, writes, recovery or restart.
        check(CoordinatorState.shouldCoordinate(false, 0)); // Explicit OFF still works.
        check(!removal.canComplete(true, "45006", false, "pending", "pending"));
        check(!removal.observe("45006"));
        check(removal.epoch() == appliedEpoch); // Verified epoch remains current; no reapply.
        check(!CoordinatorState.needsApply(appliedEpoch, 0, removal.epoch(), 0));
        removal.observe("no_sim");
        check(removal.observe("45005"));
        check(removal.epoch() != appliedEpoch && CoordinatorState.shouldCoordinate(true, 1));
        check(CoordinatorState.needsApply(appliedEpoch, 0, removal.epoch(), 0));
        check(!CoordinatorState.shouldRecordApplied("")); // Changed carrier resets before applying P.
        removal.observe("no_sim");
        check(removal.observe("45007"));
        check(CoordinatorState.shouldRestoreAutomatic("unsupported", 1, true, 0));

        CoordinatorState state = new CoordinatorState();
        check(!state.canComplete(true, "450006"));
        check(state.observe("450006"));
        check(state.epoch() == 1);
        state.setRadiosReady(true);
        // Completion requires radio recovery as well as a loaded matching identity.
        check(!state.canComplete(false, "450006"));
        check(state.canComplete(true, "450006"));
        check(!state.canComplete(true, "45006"));
        long epoch = state.epoch();
        check(!state.observe("450006"));
        check(state.epoch() == epoch);
        state.clear();
        check(!state.canComplete(true, "450006"));
        check(state.epoch() == epoch);
        check(!state.observe("450006"));
        check(state.epoch() == epoch);
        check(state.canComplete(true, "450006"));
        check(state.observe("450006;45006"));
        check(state.epoch() == epoch + 1);
        check(!state.canComplete(true, "450006"));
        check(!state.canComplete(false, "450006;45006"));
        check(state.canComplete(true, "450006;45006"));
        state.clear();
        check(!state.canComplete(true, "450006;45006"));
        check(state.observe("45005"));
        check(state.epoch() == epoch + 2);
        check(!state.canComplete(true, "450006;45006"));
        check(state.canComplete(true, "45005"));
        check(!state.observe("no_sim"));
        check(!state.canComplete(false, "no_sim"));
        check(!state.canComplete(true, "no_sim"));
        check(!CoordinatorState.rebooted("boot-a", "boot-a"));
        check(CoordinatorState.rebooted("boot-a", "boot-b"));
        check(!CoordinatorState.rebooted("", "boot-b"));
        check(!CoordinatorState.rebooted("boot-a", ""));
        check(!state.canComplete(CoordinatorState.rebooted("boot-a", "boot-b"), "450006"));
        state.clear();
        check(!state.canComplete(CoordinatorState.rebooted("boot-a", "boot-b"), "no_sim"));
        state.observe("no_sim");
        check(state.canComplete(CoordinatorState.rebooted("boot-a", "boot-b"), "", true, "off", "off"));
        epoch = state.epoch();
        check(state.retryAllowed(epoch, 0, 1000));
        state.attemptFailed(epoch, 0, 1000);
        check(!state.retryAllowed(epoch, 0, 30_999));
        check(state.retryAllowed(epoch, 0, 31_000));
        state.attemptFailed(epoch, 0, 31_000);
        check(!state.retryAllowed(epoch, 0, 150_999));
        check(state.retryAllowed(epoch, 0, 151_000));
        state.attemptFailed(epoch, 0, 151_000);
        check(!state.retryAllowed(epoch, 0, 750_999));
        check(state.retryAllowed(epoch, 0, 751_000));
        state.attemptFailed(epoch, 0, 751_000);
        check(state.retryAllowed(epoch, 0, Long.MAX_VALUE));
        state.clear();
        state.observe("no_sim");
        check(state.retryAllowed(epoch, 0, Long.MAX_VALUE));
        check(state.retryAllowed(epoch, 1, 751_001)); // Automatic switch toggled.
        state.attemptFailed(epoch, 1, 751_001);
        state.observe("450006");
        check(state.retryAllowed(state.epoch(), 1, 751_002));
        state.attemptFailed(epoch, 1, 751_002); // Stale failure cannot throttle the new SIM.
        check(state.retryAllowed(state.epoch(), 1, 751_002));
        check(!state.retryAllowed(epoch, 1, Long.MAX_VALUE));
        CoordinatorState restarted = new CoordinatorState();
        restarted.observe("450006");
        check(restarted.retryAllowed(restarted.epoch(), 1, 0));
        check(CoordinatorState.shouldRestoreUnsupported("unsupported", 1));
        check(!CoordinatorState.shouldRestoreUnsupported("unsupported", 0));
        check(!CoordinatorState.shouldRestoreUnsupported("unsupported", 2));
        check(!CoordinatorState.shouldRestoreUnsupported("conflict", 1));
        check(!CoordinatorState.shouldRestoreUnsupported("conflict", 2));
        check(!CoordinatorState.shouldRestoreUnsupported("checking", 1));
        check(CoordinatorState.shouldRestoreAutomatic("unsupported", 1, false, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 0, false, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("unsupported", 1, false, 7));
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 0, false, 7));
        check(!CoordinatorState.shouldRestoreAutomatic("unsupported", 1, false, -1));
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 0, false, -1));
        check(CoordinatorState.shouldRestoreAutomatic("unsupported", 1, true, -1));
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 0, true, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("conflict", 2, false, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 1, false, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("checking", 1, false, 0));
        check(!CoordinatorState.shouldRestoreAutomatic("restart_pending", 0, false, 0));
        // Control-only and overlay transactions both require a witnessed restart.
        check(!CoordinatorState.shouldRestoreAutomatic("waiting_for_identity", 0, true, 7));
        check(!CoordinatorState.shouldRecordApplied("")); // Q -> 07/restart still needs P.
        check(CoordinatorState.shouldRecordApplied("Korea/SKT/Commercial")); // P/restart applied.
        check(!CoordinatorState.shouldConfigureAutomatic(true, true, false));
        check(CoordinatorState.shouldConfigureAutomatic(true, true, true));
        check(CoordinatorState.shouldConfigureAutomatic(true, false, false));
        check(CoordinatorState.shouldConfigureAutomatic(false, true, false));
        check(CoordinatorState.shouldConfigureAutomatic(false, false, true));
        check(!CoordinatorState.shouldConfigureAutomatic(false, false, false));
        // Neither direction of an explicit switch request is suppressed after a failure.
        state.attemptFailed(state.epoch(), 2, 0);
        check(CoordinatorState.shouldConfigureAutomatic(true, true, true));
        check(CoordinatorState.shouldConfigureAutomatic(false, false, true));
        check(state.retryAllowed(state.epoch(), 3, 1)); // Switch resets retry version.
        System.out.println("PASS: " + checks + " coordinator identity/restart checks");
    }
}
