/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touch;

import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;

import lineageos.providers.LineageSettings;
import lineageos.hardware.LineageHardwareManager;

import vendor.lenovo.hardware.touchpolicy.ITouchPolicy;
import vendor.lenovo.hardware.touchpolicy.ITouchPolicyCallback;
import vendor.lenovo.hardware.touchpolicy.TouchSessionConfig;

/**
 * Applies the game touch policy through the touch policy HAL.
 * GameSpace publishes the fullscreen session; this class owns every value decision.
 */
final class TouchPolicyController {
    private static final String TAG = "LenovoTouchPolicy";

    static final String KEY_SESSION = "touch_policy_session";
    static final String KEY_HIGH_POLLING = "touch_sensitivity_high_polling";
    static final String KEY_TAP = "touch_sensitivity_tap";
    static final String KEY_MOVE = "touch_sensitivity_move";
    static final String KEY_EDGE = "touch_sensitivity_edge";
    static final String KEY_PEN_INPUT = "game_pen_input";
    private static final String KEY_RESTORE_POLLING = "touch_sensitivity_high_polling_restore";

    private static final int DEFAULT_TAP = 11;
    private static final int DEFAULT_MOVE = 4;
    private static final int DEFAULT_EDGE = 1;
    private static final long RETRY_MIN_MS = 1000;
    private static final long RETRY_MAX_MS = 30000;

    private final Context mContext;
    private final ContentResolver mResolver;
    private final DisplayManager mDisplayManager;
    private final Handler mHandler;
    private final PenDeviceGate mPenGate;
    private final LineageHardwareManager mHardware;

    private ITouchPolicy mHal;
    private IBinder mHalBinder;
    private boolean mCallbackRegistered;
    private boolean mSessionActive;
    private int mAppliedTap = -1;
    private int mAppliedMove = -1;
    private int mAppliedEdge = -1;
    private boolean mAppliedPenInput;
    private boolean mNeedsSync = true;
    private boolean mJournaled;
    private int mJournalUser = UserHandle.USER_NULL;
    private int mUser = UserHandle.USER_NULL;
    private int mRestorePolling = -1;
    private int mAppliedHighPolling = -1;
    private boolean mScreenOff;
    private boolean mForceEnd;
    private long mRetryDelayMs = RETRY_MIN_MS;
    private final Runnable mRetry = this::update;

    private final ITouchPolicyCallback.Stub mCallback = new ITouchPolicyCallback.Stub() {
        @Override
        public void onPenStateChanged(int state) {
            mHandler.post(TouchPolicyController.this::update);
        }

        @Override
        public int getInterfaceVersion() {
            return ITouchPolicyCallback.VERSION;
        }

        @Override
        public String getInterfaceHash() {
            return ITouchPolicyCallback.HASH;
        }
    };

    private final ContentObserver mObserver = new ContentObserver(null) {
        @Override
        public void onChange(boolean selfChange) {
            mHandler.post(TouchPolicyController.this::update);
        }
    };

    private final ContentObserver mPollingObserver = new ContentObserver(null) {
        @Override
        public void onChange(boolean selfChange) {
            mHandler.post(TouchPolicyController.this::onLineagePollingChanged);
        }
    };

    private final DisplayManager.DisplayListener mDisplayListener =
            new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int displayId) {
        }

        @Override
        public void onDisplayRemoved(int displayId) {
        }

        @Override
        public void onDisplayChanged(int displayId) {
            if (displayId == Display.DEFAULT_DISPLAY) mHandler.post(TouchPolicyController.this::update);
        }
    };

    private final IBinder.DeathRecipient mHalDeath = this::onHalDied;

    private void onHalDied() {
        mHandler.post(() -> {
            Log.w(TAG, "The touch policy HAL died; re-syncing");
            mHal = null;
            mHalBinder = null;
            mCallbackRegistered = false;
            mNeedsSync = true;
            update();
        });
    }

    private final Runnable mScreenOnUpdate = () -> {
        mScreenOff = false;
        mForceEnd = false;
        update();
    };

    private final BroadcastReceiver mScreenReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_USER_SWITCHED.equals(intent.getAction())) {
                mHandler.post(TouchPolicyController.this::update);
            } else if (Intent.ACTION_SCREEN_OFF.equals(intent.getAction())) {
                mHandler.removeCallbacks(mScreenOnUpdate);
                mHandler.post(() -> {
                    mScreenOff = true;
                    mForceEnd = true;
                    update();
                });
            } else if (Intent.ACTION_SCREEN_ON.equals(intent.getAction())) {
                mHandler.removeCallbacks(mScreenOnUpdate);
                mHandler.postDelayed(mScreenOnUpdate, 500);
            }
        }
    };

    TouchPolicyController(Context context, Handler handler) {
        mContext = context;
        mResolver = context.getContentResolver();
        mDisplayManager = context.getSystemService(DisplayManager.class);
        mHandler = handler;
        mPenGate = new PenDeviceGate(context, mHandler);
        mHardware = LineageHardwareManager.getInstance(context);
    }

    void start() {
        for (String key : new String[] {KEY_SESSION, KEY_HIGH_POLLING, KEY_TAP, KEY_MOVE,
                KEY_EDGE, KEY_PEN_INPUT}) {
            mResolver.registerContentObserver(Settings.Secure.getUriFor(key), false, mObserver,
                    UserHandle.USER_ALL);
        }
        mResolver.registerContentObserver(
                LineageSettings.System.getUriFor(
                        LineageSettings.System.HIGH_TOUCH_POLLING_RATE_ENABLE),
                false, mPollingObserver, UserHandle.USER_ALL);
        mDisplayManager.registerDisplayListener(mDisplayListener, mHandler);
        IntentFilter screenFilter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        screenFilter.addAction(Intent.ACTION_SCREEN_ON);
        screenFilter.addAction(Intent.ACTION_USER_SWITCHED);
        mContext.registerReceiver(mScreenReceiver, screenFilter, Context.RECEIVER_NOT_EXPORTED);
        // A previous process may have died with the pen blocked.
        mHandler.post(() -> {
            mPenGate.setBlocked(false);
            // Observers may already have applied the live session on this handler.
            // Recovery must not leave that session's gate open until its next change.
            mNeedsSync = true;
            update();
        });
    }

    void stop() {
        mResolver.unregisterContentObserver(mObserver);
        mResolver.unregisterContentObserver(mPollingObserver);
        mDisplayManager.unregisterDisplayListener(mDisplayListener);
        mContext.unregisterReceiver(mScreenReceiver);
        ITouchPolicy hal = mHal;
        mHal = null;
        if (hal != null) {
            try {
                hal.unregisterCallback(mCallback);
            } catch (Exception e) {
                Log.w(TAG, "Cannot unregister the pen callback", e);
            }
        }
    }

    private int currentUser() {
        return ActivityManager.getCurrentUser();
    }

    private ITouchPolicy hal() {
        if (mHal != null) return mHal;
        mCallbackRegistered = false;
        IBinder binder = ServiceManager.waitForDeclaredService(
                ITouchPolicy.DESCRIPTOR + "/default");
        if (binder == null) return null;
        try {
            binder.linkToDeath(mHalDeath, 0);
        } catch (RemoteException e) {
            Log.w(TAG, "Cannot link to the touch policy HAL", e);
            return null;
        }
        ITouchPolicy hal = ITouchPolicy.Stub.asInterface(binder);
        if (hal == null) {
            unlinkDeath(binder);
            return null;
        }
        try {
            hal.registerCallback(mCallback);
            mCallbackRegistered = true;
        } catch (Exception e) {
            Log.w(TAG, "Cannot register the pen callback", e);
            unlinkDeath(binder);
            return null;
        }
        mHalBinder = binder;
        mHal = hal;
        return hal;
    }

    private void scheduleRetry() {
        mHandler.removeCallbacks(mRetry);
        mHandler.postDelayed(mRetry, mRetryDelayMs);
        mRetryDelayMs = Math.min(mRetryDelayMs * 2, RETRY_MAX_MS);
    }

    private void resetRetry() {
        mHandler.removeCallbacks(mRetry);
        mRetryDelayMs = RETRY_MIN_MS;
    }

    private void dropHal() {
        IBinder binder = mHalBinder;
        mHal = null;
        mHalBinder = null;
        mCallbackRegistered = false;
        mNeedsSync = true;
        if (binder != null) unlinkDeath(binder);
    }

    private void unlinkDeath(IBinder binder) {
        try {
            binder.unlinkToDeath(mHalDeath, 0);
        } catch (RuntimeException e) {
            // The binder is already dead.
        }
    }

    void onRotationChanged() {
        // Rotation and policy share this handler; resend after the direction write.
        if (!mSessionActive) return;
        mNeedsSync = true;
        update();
    }

    private void update() {
        final int user = currentUser();
        if (mUser != user) {
            if (mUser != UserHandle.USER_NULL && !endSession()) return;
            mUser = user;
            loadJournal(user);
        }
        if (mForceEnd) {
            if (endSession()) mForceEnd = false;
            return;
        }
        if (mScreenOff) return;
        final String session = Settings.Secure.getStringForUser(mResolver, KEY_SESSION, user);
        final boolean active = session != null && !session.isEmpty();
        if (!active) {
            if (mSessionActive || mNeedsSync || mJournaled) endSession();
            return;
        }
        ITouchPolicy hal = hal();
        if (hal == null) {
            // The vendor service may still be starting; retry while the session is live.
            scheduleRetry();
            return;
        }
        final int tap = clamp(
                Settings.Secure.getIntForUser(mResolver, KEY_TAP, DEFAULT_TAP, user), 1, 21);
        final int move = clamp(
                Settings.Secure.getIntForUser(mResolver, KEY_MOVE, DEFAULT_MOVE, user), 2, 6);
        final int edge = clamp(
                Settings.Secure.getIntForUser(mResolver, KEY_EDGE, DEFAULT_EDGE, user), 0, 3);
        final boolean penInput =
                Settings.Secure.getIntForUser(mResolver, KEY_PEN_INPUT, 0, user) == 1;
        final boolean highPollingOn =
                Settings.Secure.getIntForUser(mResolver, KEY_HIGH_POLLING, 1, user) == 1;
        final int highPolling = !penInput && highPollingOn ? 1 : 0;
        final int pollingFeature = LineageHardwareManager.FEATURE_HIGH_TOUCH_POLLING_RATE;
        final boolean changed = mNeedsSync || !mSessionActive || tap != mAppliedTap ||
                move != mAppliedMove || edge != mAppliedEdge ||
                penInput != mAppliedPenInput || highPolling != mAppliedHighPolling ||
                mHardware.get(pollingFeature) != (highPolling == 1);
        if (changed) {
            mNeedsSync = true;
            journalHighPolling(user);
            // Game parameters depend on the report rate, even on a same-value resend.
            if (!applyHighPolling(user, highPolling)) return;
            try {
                TouchSessionConfig config = new TouchSessionConfig();
                config.tapSensitivity = tap;
                config.moveSensitivity = move;
                config.edgeRejection = edge;
                hal.setSessionPenInput(penInput);
                hal.setSession(config);
                mPenGate.setBlocked(!penInput);
                Log.d(TAG, "Applied session tap=" + tap + " move=" + move +
                        " edge=" + edge + " penInput=" + penInput);
            } catch (UnsupportedOperationException e) {
                Log.w(TAG, "Touch node write failed; retrying", e);
                scheduleRetry();
                return;
            } catch (Exception e) {
                Log.w(TAG, "Cannot apply the touch session", e);
                dropHal();
                scheduleRetry();
                return;
            }
            mAppliedTap = tap;
            mAppliedMove = move;
            mAppliedEdge = edge;
            mAppliedPenInput = penInput;
            mSessionActive = true;
            mNeedsSync = false;
            resetRetry();
        }
    }

    /** Returns true when the HAL restored the idle state. */
    private boolean endSession() {
        ITouchPolicy hal = hal();
        if (hal == null) {
            // Keep the journal and the live flag; update retries the restore.
            scheduleRetry();
            return false;
        }
        mPenGate.setBlocked(false);
        try {
            hal.endSession();
            hal.setSessionPenInput(false);
            Log.d(TAG, "Cleared touch session");
        } catch (UnsupportedOperationException e) {
            Log.w(TAG, "Touch node restore failed; retrying", e);
            scheduleRetry();
            return false;
        } catch (Exception e) {
            Log.w(TAG, "Cannot end the touch session", e);
            dropHal();
            scheduleRetry();
            return false;
        }
        resetRetry();
        if (!restoreHighPolling()) {
            scheduleRetry();
            return false;
        }
        mSessionActive = false;
        mNeedsSync = false;
        mAppliedTap = -1;
        mAppliedMove = -1;
        mAppliedEdge = -1;
        mAppliedPenInput = false;
        return true;
    }

    private void loadJournal(int user) {
        int value = Settings.Secure.getIntForUser(mResolver, KEY_RESTORE_POLLING, -1, user);
        if (value >= 0) {
            mRestorePolling = value;
            mJournalUser = user;
            mJournaled = true;
        }
    }

    private void journalHighPolling(int user) {
        if (mJournaled) return;
        mRestorePolling = LineageSettings.System.getIntForUser(mResolver,
                LineageSettings.System.HIGH_TOUCH_POLLING_RATE_ENABLE, 0, user);
        Settings.Secure.putIntForUser(mResolver, KEY_RESTORE_POLLING, mRestorePolling, user);
        mJournalUser = user;
        mJournaled = true;
    }

    private boolean restoreHighPolling() {
        if (!mJournaled) return true;
        if (!LineageSettings.System.putIntForUser(mResolver,
                LineageSettings.System.HIGH_TOUCH_POLLING_RATE_ENABLE,
                mRestorePolling, mJournalUser)) return false;
        final int feature = LineageHardwareManager.FEATURE_HIGH_TOUCH_POLLING_RATE;
        mHardware.set(feature, mRestorePolling == 1);
        if (mHardware.get(feature) != (mRestorePolling == 1)) return false;
        Log.d(TAG, "Restored report rate=" + mRestorePolling);
        if (!Settings.Secure.putIntForUser(mResolver, KEY_RESTORE_POLLING, -1,
                mJournalUser)) return false;
        mJournaled = false;
        mJournalUser = UserHandle.USER_NULL;
        mAppliedHighPolling = -1;
        return true;
    }

    private void onLineagePollingChanged() {
        // Keep adopting the user's value while a restore journal is still held.
        if (!mSessionActive && !mJournaled) return;
        int user = currentUser();
        if (user != mJournalUser) {
            update();
            return;
        }
        int value = LineageSettings.System.getIntForUser(mResolver,
                LineageSettings.System.HIGH_TOUCH_POLLING_RATE_ENABLE, 0, user);
        if (value == mAppliedHighPolling) return;
        // A user change mid-session becomes the value restored after the session.
        mRestorePolling = value;
        mJournaled = true;
        Settings.Secure.putIntForUser(mResolver, KEY_RESTORE_POLLING, value, user);
        // Re-apply the session decision; the adopted value is only the restore target.
        mAppliedHighPolling = -1;
        update();
    }

    private boolean applyHighPolling(int user, int value) {
        // The controller's polling mode also selects pen compatibility: high reporting
        // blocks the pen, so pen input forces it off. Keep the user's polling choice in
        // the journal; with the switch off the input device gate alone blocks the pen.
        final int feature = LineageHardwareManager.FEATURE_HIGH_TOUCH_POLLING_RATE;
        final boolean enabled = value == 1;
        // Do not rely on the IME's optional settings observer to apply the firmware mode.
        // The AIDL setter returns the requested state, including false on disable.
        // Read back the mode to decide whether the write succeeded.
        mHardware.set(feature, enabled);
        if (mHardware.get(feature) != enabled) {
            Log.w(TAG, "Cannot apply the pen firmware mode");
            scheduleRetry();
            return false;
        }
        if (!LineageSettings.System.putIntForUser(mResolver,
                LineageSettings.System.HIGH_TOUCH_POLLING_RATE_ENABLE, value, user)) {
            scheduleRetry();
            return false;
        }
        mAppliedHighPolling = value;
        Log.d(TAG, "Applied report rate=" + value);
        return true;
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
