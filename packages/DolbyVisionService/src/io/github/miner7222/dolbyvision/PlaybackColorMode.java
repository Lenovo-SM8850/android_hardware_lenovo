/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.database.ContentObserver;
import android.hardware.display.ColorDisplayManager;
import android.net.Uri;
import android.os.Handler;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;

import java.util.Collection;

/** Owns a temporary playback color mode, with durable recovery for its original user. */
final class PlaybackColorMode {
    private final int mPlaybackMode;
    private static final long STOP_DELAY_MS = 150;

    private final Context mContext;
    private final Handler mHandler;
    private final ColorDisplayManager mColorDisplayManager;
    private final int[] mAvailableModes;
    private boolean mEnabled;
    private boolean mPlaying;
    private final ColorModeOverride mOverride;
    private ContentObserver mObserver;
    private final Runnable mRestore = () -> {
        if (!mPlaying) {
            restorePending();
        }
    };

    private final BroadcastReceiver mUserSwitchReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            stop(true);
        }
    };

    PlaybackColorMode(Context context, Handler handler) {
        mContext = context;
        mPlaybackMode = context.getResources().getInteger(R.integer.config_playbackColorMode);
        mHandler = handler;
        mColorDisplayManager = context.getSystemService(ColorDisplayManager.class);
        mAvailableModes = context.getResources().getIntArray(
                com.android.internal.R.array.config_availableColorModes);
        final SharedPreferences journal = context.createDeviceProtectedStorageContext()
                .getSharedPreferences("playback_color_mode", Context.MODE_PRIVATE);
        mOverride = new ColorModeOverride(new ColorModeOverride.Store() {
            @Override
            public ColorModeOverride.Marker load() {
                if (!journal.contains("user")) {
                    return null;
                }
                return new ColorModeOverride.Marker(journal.getInt("user", -1),
                        journal.getInt("mode", -1), journal.getLong("generation", 0));
            }

            @Override
            public boolean save(ColorModeOverride.Marker marker) {
                final boolean saved = journal.edit().putInt("user", marker.user)
                        .putInt("mode", marker.originalMode)
                        .putLong("generation", marker.generation).commit();
                if (!saved) {
                    Log.w(DolbyVisionService.TAG, "Cannot persist playback color mode restore state");
                }
                return saved;
            }

            @Override
            public boolean clear() {
                final boolean cleared = journal.edit().remove("user").remove("mode").commit();
                if (!cleared) {
                    Log.w(DolbyVisionService.TAG, "Cannot clear playback color mode restore state");
                }
                return cleared;
            }
        }, new ColorModeOverride.Modes() {
            @Override
            public int read(int user) {
                return readMode(user);
            }

            @Override
            public boolean write(int user, int mode) {
                // ColorDisplayManager writes this setting; explicit users avoid switch races.
                final boolean written = Settings.System.putIntForUser(context.getContentResolver(),
                        Settings.System.DISPLAY_COLOR_MODE, mode, user);
                if (written) {
                    Log.i(DolbyVisionService.TAG, "Set color mode " + mode + " for user " + user);
                } else {
                    Log.w(DolbyVisionService.TAG, "Cannot write playback color mode");
                }
                return written;
            }
        }, mPlaybackMode);
        // Recover before registering with DVS, even if the daemon is not available yet.
        restorePending();
        context.registerReceiverAsUser(mUserSwitchReceiver, UserHandle.ALL,
                new IntentFilter(Intent.ACTION_USER_SWITCHED), null, handler);
    }

    void setEnabled(boolean enabled) {
        mEnabled = enabled && mPlaybackMode >= 0 && isAvailable(mPlaybackMode);
        if (!mEnabled) {
            stop(true);
        }
    }

    void start(boolean replayed) {
        mHandler.removeCallbacks(mRestore);
        if (mPlaying || !mEnabled) {
            return;
        }
        mPlaying = true;
        if (replayed) {
            // DVS reports existing playback during registration, including after crash recovery.
            Log.i(DolbyVisionService.TAG, "Keeping color mode for replayed playback start");
            return;
        }
        if (mOverride.pending() != null) {
            // A rapid restart retains the original mode across the stop grace period.
            return;
        }
        final int user = ActivityManager.getCurrentUser();
        final int mode = mColorDisplayManager.getColorMode();
        if (mode == mPlaybackMode || !isAvailable(mode)) {
            return;
        }
        final int stored = readMode(user);
        if (user != ActivityManager.getCurrentUser() || (stored >= 0 && stored != mode)) {
            return;
        }
        mOverride.start(user, mode);
        final ColorModeOverride.Marker marker = mOverride.pending();
        if (marker == null) {
            return;
        }
        mObserver = new ContentObserver(mHandler) {
            @Override
            public void onChange(boolean selfChange, Collection<Uri> uris, int flags, int userId) {
                if (mObserver != this || userId != marker.user) {
                    return;
                }
                mOverride.observe(marker.generation);
                removeObserverIfCleared();
            }
        };
        mContext.getContentResolver().registerContentObserver(
                Settings.System.getUriFor(Settings.System.DISPLAY_COLOR_MODE),
                false, mObserver, user);
        // Catch a choice made between applying the override and observer registration.
        mOverride.observe(marker.generation);
        removeObserverIfCleared();
    }

    void stop(boolean immediate) {
        mPlaying = false;
        mHandler.removeCallbacks(mRestore);
        if (immediate) {
            restorePending();
        } else if (mOverride.pending() != null) {
            mHandler.postDelayed(mRestore, STOP_DELAY_MS);
        }
    }

    void destroy() {
        stop(true);
        mContext.unregisterReceiver(mUserSwitchReceiver);
    }

    private boolean isAvailable(int mode) {
        for (int available : mAvailableModes) {
            if (available == mode) {
                return true;
            }
        }
        return false;
    }

    private int readMode(int user) {
        return Settings.System.getIntForUser(mContext.getContentResolver(),
                Settings.System.DISPLAY_COLOR_MODE, -1, user);
    }

    private void restorePending() {
        mOverride.restore();
        removeObserverIfCleared();
    }

    private void removeObserverIfCleared() {
        if (mOverride.pending() == null && mObserver != null) {
            mContext.getContentResolver().unregisterContentObserver(mObserver);
            mObserver = null;
        }
    }
}
