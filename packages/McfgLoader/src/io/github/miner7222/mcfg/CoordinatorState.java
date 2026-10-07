// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.mcfg;

import java.util.HashSet;
import java.util.Set;

/** Monotonic time gates, independent of Android Binder and wall-clock changes. */
final class CoordinatorState {
    private String identity = "";
    private long stableSince, emergencyHoldUntil, restartNotBefore, epoch;

    synchronized boolean observe(String value, long now) {
        if (value.equals(identity)) return false;
        identity = value;
        invalidate(now);
        return true;
    }

    synchronized void invalidate(long now) {
        stableSince = now;
        ++epoch;
    }

    synchronized void clear(long now) {
        identity = "";
        invalidate(now);
    }

    synchronized void emergency(long now) {
        emergencyHoldUntil = Math.max(emergencyHoldUntil, now + 300_000);
        invalidate(now);
    }

    synchronized void restarting(long now) {
        restartNotBefore = now + 30_000;
        invalidate(now);
    }

    synchronized boolean stable(long now) {
        return !identity.isEmpty() && now - stableSince >= 30_000 && now >= emergencyHoldUntil;
    }

    synchronized boolean canComplete(long now, boolean sawRadioDown) {
        return sawRadioDown && stable(now) && now >= restartNotBefore;
    }

    synchronized long epoch() {
        return epoch;
    }

    static final class EmergencySnapshot {
        private final Set<Integer> seen = new HashSet<>(), active = new HashSet<>();
        private boolean invalid;

        synchronized void update(int type, boolean entered) {
            if (type != 1 && type != 2) {
                invalid = true;
                return;
            }
            seen.add(type);
            if (entered) active.add(type);
            else active.remove(type);
        }

        synchronized boolean knownAndClear() {
            return !invalid && seen.contains(1) && seen.contains(2) && active.isEmpty();
        }
    }
}
