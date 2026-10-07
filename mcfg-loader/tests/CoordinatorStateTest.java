// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

public final class CoordinatorStateTest {
    private static int checks;

    private static void check(boolean ok) {
        ++checks;
        if (!ok) throw new AssertionError("check " + checks);
    }

    public static void main(String[] args) {
        CoordinatorState state = new CoordinatorState();
        check(!state.stable(1_000_000));
        check(state.observe("slot0:450006;", 1000));
        check(!state.stable(30_999));
        check(state.stable(31_000));
        long epoch = state.epoch();
        check(!state.observe("slot0:450006;", 32_000));
        check(state.epoch() == epoch);
        check(state.observe("slot0:450006;slot1:45006;", 33_000));
        check(!state.stable(34_000));
        state.invalidate(40_000);
        check(!state.stable(69_999));
        check(state.stable(70_000));
        state.emergency(80_000);
        check(!state.stable(379_999));
        check(state.stable(380_000));
        state.emergency(390_000);
        state.invalidate(395_000);
        check(!state.stable(689_999));
        check(state.stable(690_000));
        state.restarting(700_000);
        check(!state.canComplete(729_999, true));
        check(!state.canComplete(730_000, false));
        check(state.canComplete(730_000, true));
        state.clear(740_000);
        check(!state.stable(1_000_000));
        check(state.observe("slot0:450006;", 1_010_000));
        check(!state.stable(1_020_000));
        check(state.stable(1_040_000));
        CoordinatorState.EmergencySnapshot emergency = new CoordinatorState.EmergencySnapshot();
        check(!emergency.knownAndClear());
        emergency.update(1, false);
        check(!emergency.knownAndClear());
        emergency.update(2, false);
        check(emergency.knownAndClear());
        emergency.update(1, true);
        check(!emergency.knownAndClear());
        emergency.update(2, true);
        emergency.update(1, false);
        check(!emergency.knownAndClear());
        emergency.update(2, false);
        check(emergency.knownAndClear());
        emergency.update(3, false);
        check(!emergency.knownAndClear());
        System.out.println("PASS: " + checks + " coordinator debounce/emergency/restart checks");
    }
}
