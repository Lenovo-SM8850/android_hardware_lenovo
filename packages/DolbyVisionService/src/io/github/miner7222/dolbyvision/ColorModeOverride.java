/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

/** Serial, value-based ownership of a journaled color mode override. */
final class ColorModeOverride {
    private final int mPlaybackMode;

    static final class Marker {
        final int user;
        final int originalMode;
        final long generation;

        Marker(int user, int originalMode, long generation) {
            this.user = user;
            this.originalMode = originalMode;
            this.generation = generation;
        }
    }

    interface Store {
        Marker load();
        boolean save(Marker marker);
        boolean clear();
    }

    interface Modes {
        int read(int user);
        boolean write(int user, int mode);
    }

    private final Store mStore;
    private final Modes mModes;
    private Marker mPending;
    private long mGeneration;

    ColorModeOverride(Store store, Modes modes, int playbackMode) {
        mPlaybackMode = playbackMode;
        mStore = store;
        mModes = modes;
        mPending = store.load();
        mGeneration = Math.max(System.nanoTime(), mPending == null ? 0 : mPending.generation);
    }

    Marker pending() {
        return mPending;
    }

    void start(int user, int originalMode) {
        if (mPending != null || originalMode == mPlaybackMode) {
            return;
        }
        final Marker marker = new Marker(user, originalMode, ++mGeneration);
        // A failed synchronous journal write must never be followed by an override.
        if (!mStore.save(marker)) {
            return;
        }
        mPending = marker;
        if (!mModes.write(user, mPlaybackMode)) {
            restore();
        }
    }

    void observe(long generation) {
        if (mPending != null && mPending.generation == generation
                && mModes.read(mPending.user) != mPlaybackMode) {
            clear();
        }
        // Reading the playback mode says nothing about the author or order of notifications.
    }

    void restore() {
        if (mPending == null) {
            return;
        }
        if (mModes.read(mPending.user) == mPlaybackMode
                && !mModes.write(mPending.user, mPending.originalMode)) {
            return;
        }
        clear();
    }

    private void clear() {
        if (mStore.clear()) {
            mPending = null;
        }
    }
}
