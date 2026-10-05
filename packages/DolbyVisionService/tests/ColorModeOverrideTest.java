/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import java.util.ArrayList;
import java.util.List;

/** Host tests exercise the production ownership state with reordered Settings notifications. */
public final class ColorModeOverrideTest {
    private static int checks;

    private static void check(boolean value) {
        checks++;
        if (!value) {
            throw new AssertionError("Check " + checks);
        }
    }

    private static final class Fixture implements ColorModeOverride.Store, ColorModeOverride.Modes {
        ColorModeOverride.Marker disk;
        int mode = 257;
        int lastUser = -1;
        boolean failSave;
        boolean failClear;
        boolean failWrite;
        Runnable beforeWrite;
        final List<String> events = new ArrayList<>();
        ColorModeOverride state = new ColorModeOverride(this, this, 300);

        @Override
        public ColorModeOverride.Marker load() {
            return disk;
        }

        @Override
        public boolean save(ColorModeOverride.Marker marker) {
            events.add("commit marker");
            if (failSave) return false;
            disk = marker;
            return true;
        }

        @Override
        public boolean clear() {
            events.add("commit clear");
            if (failClear) return false;
            disk = null;
            return true;
        }

        @Override
        public int read(int user) {
            lastUser = user;
            return mode;
        }

        @Override
        public boolean write(int user, int value) {
            lastUser = user;
            if (beforeWrite != null) {
                Runnable action = beforeWrite;
                beforeWrite = null;
                action.run();
            }
            if (value == 300) check(disk != null && disk.user == user);
            events.add("write " + value);
            if (failWrite) return false;
            mode = value;
            return true;
        }

        long start() {
            state.start(10, 257);
            return state.pending().generation;
        }

        void restart() {
            state = new ColorModeOverride(this, this, 300);
            state.restore();
        }
    }

    private static void reorderedNotifications() {
        // Queued notifications carry no author or old value; every callback reads current state.
        for (int count = 0; count < 5; count++) {
            Fixture f = new Fixture();
            long generation = f.start();
            for (int i = 0; i < count; i++) f.state.observe(generation);
            check(f.disk != null && f.mode == 300);
            f.state.restore();
            for (int i = 0; i < count; i++) f.state.observe(generation);
            check(f.disk == null && f.mode == 257);
            check(f.events.equals(List.of("commit marker", "write 300", "write 257", "commit clear")));
        }
        Fixture f = new Fixture();
        long generation = f.start();
        f.mode = 0;
        // The user's notification arrives before the notification from our write.
        f.state.observe(generation);
        check(f.disk == null && f.mode == 0);
        f.state.observe(generation);
        f.state.restore();
        check(f.mode == 0);

        f = new Fixture();
        generation = f.start();
        f.state.observe(generation);
        f.mode = 0;
        f.state.observe(generation);
        f.mode = 300;
        f.state.observe(generation);
        f.state.restore();
        check(f.mode == 300 && f.disk == null);

        // An unobserved away/back pair reads 300 and must not infer ownership loss.
        f = new Fixture();
        generation = f.start();
        f.mode = 0;
        f.mode = 300;
        f.state.observe(generation);
        f.state.observe(generation);
        check(f.disk != null);
        f.state.restore();
        check(f.mode == 257 && f.disk == null);
    }

    private static void racingWrites() {
        Fixture f = new Fixture();
        final Fixture start = f;
        f.beforeWrite = () -> start.mode = 0;
        long generation = f.start();
        f.state.observe(generation);
        check(f.mode == 300 && f.disk != null);
        f.state.restore();
        check(f.mode == 257 && f.disk == null);

        f = new Fixture();
        f.start();
        final Fixture restore = f;
        f.beforeWrite = () -> restore.mode = 0;
        // Without Settings CAS, this instant can restore the saved original over a user write.
        f.state.restore();
        check(f.mode == 257 && f.disk == null);

        f = new Fixture();
        f.start();
        f.mode = 0;
        // Stop beats a delayed notification: read the value and preserve the choice.
        f.state.restore();
        check(f.mode == 0 && f.disk == null && f.events.size() == 3);
    }

    private static void failuresAndRecovery() {
        Fixture f = new Fixture();
        f.failSave = true;
        f.state.start(10, 257);
        check(f.mode == 257 && f.state.pending() == null && f.events.size() == 1);

        f = new Fixture();
        f.start();
        f.failWrite = true;
        f.state.restore();
        check(f.mode == 300 && f.disk != null);
        f.failWrite = false;
        f.restart();
        check(f.mode == 257 && f.disk == null && f.lastUser == 10);

        f = new Fixture();
        f.start();
        f.failClear = true;
        f.state.restore();
        check(f.mode == 257 && f.disk != null && f.state.pending() != null);
        f.failClear = false;
        f.restart();
        check(f.mode == 257 && f.disk == null);

        // Crash/power loss before the override: only the synchronously committed journal exists.
        f = new Fixture();
        f.disk = new ColorModeOverride.Marker(10, 257, 50);
        f.restart();
        check(f.mode == 257 && f.disk == null && f.events.equals(List.of("commit clear")));

        // Settings may persist 300, but cannot precede the completed independent journal commit.
        f = new Fixture();
        f.start();
        f.restart();
        check(f.mode == 257 && f.disk == null && f.lastUser == 10);

        f = new Fixture();
        f.start();
        f.mode = 0;
        f.restart();
        check(f.mode == 0 && f.disk == null);

        f = new Fixture();
        f.mode = 300;
        f.restart();
        check(f.mode == 300 && f.events.isEmpty());
    }

    private static void generationsAndDuplicateStarts() {
        Fixture f = new Fixture();
        long old = f.start();
        f.state.start(11, 0);
        check(f.disk.user == 10 && f.disk.originalMode == 257 && f.disk.generation == old);
        f.state.restore();
        long current = f.start();
        check(current > old);
        f.mode = 0;
        f.state.observe(old);
        check(f.disk != null);
        f.state.observe(current);
        check(f.disk == null);

        f = new Fixture();
        f.mode = 300;
        f.state.start(10, 300);
        check(f.disk == null && f.events.isEmpty());
    }

    public static void main(String[] args) {
        reorderedNotifications();
        racingWrites();
        failuresAndRecovery();
        generationsAndDuplicateStarts();
        System.out.println("COLOR_MODE_OVERRIDE_TESTS_OK: " + checks + " checks");
    }
}
