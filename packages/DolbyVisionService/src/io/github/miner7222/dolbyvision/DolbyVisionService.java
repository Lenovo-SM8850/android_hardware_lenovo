/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.hardware.display.BrightnessInfo;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;

import com.android.internal.display.BrightnessSynchronizer;

/** Feeds panel brightness to the Dolby Vision daemon during playback. */
public class DolbyVisionService extends Service {
    static final String TAG = "LenovoDolbyVision";

    // Silent channels hide their status bar icon, so the channel alerts without sound or vibration.
    private static final String PQ_CHANNEL_ID = "dolby_vision_status";
    private static final String OLD_PQ_CHANNEL_ID = "dolby_vision_picture_mode";
    private static final int PQ_NOTIFICATION_ID = 1;
    private static final String ACTION_SET_PQ_MODE =
            "io.github.miner7222.dolbyvision.action.SET_PQ_MODE";
    private static final String EXTRA_PQ_MODE = "pqMode";
    // Order of the notification actions; the values are the daemon's pqMode numbers.
    private static final int[] PQ_MODES = {
        DvsClient.PQ_MODE_DARK, DvsClient.PQ_MODE_BRIGHT, DvsClient.PQ_MODE_VIVID
    };

    private static final long RECONNECT_DELAY_MS = 1000;
    // Log the "daemon not available" retry only now and then instead of once a second.
    private static final int RECONNECT_LOG_INTERVAL = 30;

    private HandlerThread mThread;
    private Handler mHandler;
    private DisplayManager mDisplayManager;
    private NotificationManager mNotificationManager;
    private PlaybackColorMode mPlaybackColorMode;
    // The binder callback reads this while registerCallback() is in progress.
    private volatile boolean mRegisteringCallback;

    // The fields below are only touched on mHandler's thread.
    private IBinder mDvs;
    private DvsDeathRecipient mDeathRecipient;
    private boolean mPlaying;
    private int mLastBrightness = -1;
    private boolean mBrightnessListenerRegistered;
    private boolean mAdjustedBrightnessEnabled;
    private ContentObserver mBrightnessSettingObserver;
    private AdjustedBrightnessRamp mAdjustedBrightnessRamp;
    private float mAdjustedBrightnessTarget = Float.NaN;
    private int mLastAdjustedBrightness = -1;
    private int mDisplayState = Display.STATE_UNKNOWN;
    private int mConnectAttempts;

    private final DvsCallbackBinder mCallback =
            new DvsCallbackBinder(
                    new DvsCallbackBinder.Listener() {
                        @Override
                        public void onPlaybackStart() {
                            Log.d(TAG, "Callback: Dolby Vision playback start");
                            final boolean replayed = mRegisteringCallback;
                            mHandler.post(() -> handlePlaybackStart(replayed));
                        }

                        @Override
                        public void onPlaybackStop() {
                            Log.d(TAG, "Callback: Dolby Vision playback stop");
                            mHandler.post(DolbyVisionService.this::handlePlaybackStop);
                        }
                    });

    /** Handles picture mode actions on the service handler thread. */
    private final BroadcastReceiver mPqModeReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    if (!ACTION_SET_PQ_MODE.equals(intent.getAction())) {
                        return;
                    }
                    // Already on mHandler's thread, see registerReceiver() in onCreate().
                    handleSetPqMode(intent.getIntExtra(EXTRA_PQ_MODE, -1));
                }
            };

    private final Runnable mConnectRunnable = this::connect;

    private final DisplayManager.DisplayListener mBrightnessListener =
            new DisplayManager.DisplayListener() {
                @Override
                public void onDisplayAdded(int displayId) {
                    onDisplayChanged(displayId);
                }

                @Override
                public void onDisplayRemoved(int displayId) {
                    if (displayId == Display.DEFAULT_DISPLAY) {
                        resetAdjustedBrightness();
                    }
                }

                @Override
                public void onDisplayChanged(int displayId) {
                    if (displayId == Display.DEFAULT_DISPLAY) {
                        if (mPlaying) {
                            pushBrightness();
                        }
                        updateAdjustedBrightness(false);
                    }
                }
            };

    /** Death recipient bound to the specific daemon binder it was linked to. */
    private final class DvsDeathRecipient implements IBinder.DeathRecipient {
        private final IBinder mBinder;

        DvsDeathRecipient(IBinder binder) {
            mBinder = binder;
        }

        @Override
        public void binderDied() {
            mHandler.post(() -> handleDvsDied(mBinder));
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mThread = new HandlerThread(TAG);
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        mDisplayManager = getSystemService(DisplayManager.class);
        mHandler.post(() -> {
            if (UserHandle.myUserId() == UserHandle.USER_SYSTEM) {
                mPlaybackColorMode = new PlaybackColorMode(this, mHandler);
            }
        });
        mNotificationManager = getSystemService(NotificationManager.class);
        mNotificationManager.deleteNotificationChannel(OLD_PQ_CHANNEL_ID);
        final NotificationChannel channel =
                new NotificationChannel(
                        PQ_CHANNEL_ID,
                        getString(R.string.notification_channel),
                        NotificationManager.IMPORTANCE_DEFAULT);
        channel.setSound(null, null);
        channel.enableVibration(false);
        channel.enableLights(false);
        channel.setShowBadge(false);
        mNotificationManager.createNotificationChannel(channel);
        registerReceiver(
                mPqModeReceiver,
                new IntentFilter(ACTION_SET_PQ_MODE),
                null,
                mHandler,
                Context.RECEIVER_NOT_EXPORTED);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        mHandler.post(this::connect);
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(mPqModeReceiver);
        // Runs on the handler thread before it quits, so nothing races with the teardown.
        mHandler.post(
                () -> {
                    mHandler.removeCallbacks(mConnectRunnable);
                    handlePlaybackStop();
                    stopAdjustedBrightnessFeedback();
                    if (mPlaybackColorMode != null) {
                        mPlaybackColorMode.destroy();
                    }
                    if (mDvs != null) {
                        mDvs.unlinkToDeath(mDeathRecipient, 0);
                        mDvs = null;
                        mDeathRecipient = null;
                    }
                });
        mThread.quitSafely();
        super.onDestroy();
    }

    // Connection handling

    private void connect() {
        if (mDvs != null) {
            return;
        }
        mHandler.removeCallbacks(mConnectRunnable);

        // The getService() call does not block; the daemon may simply not be registered yet.
        final IBinder dvs = ServiceManager.getService(DvsClient.SERVICE_NAME);
        if (dvs == null || !dvs.pingBinder()) {
            if (mConnectAttempts++ % RECONNECT_LOG_INTERVAL == 0) {
                Log.w(TAG, DvsClient.SERVICE_NAME + " not available, retrying");
            }
            scheduleConnect();
            return;
        }

        final DvsDeathRecipient deathRecipient = new DvsDeathRecipient(dvs);
        try {
            dvs.linkToDeath(deathRecipient, 0);
        } catch (RemoteException e) {
            Log.w(TAG, "linkToDeath failed, daemon already gone", e);
            scheduleConnect();
            return;
        }
        try {
            mRegisteringCallback = true;
            DvsClient.registerCallback(dvs, mCallback);
        } catch (RemoteException | RuntimeException e) {
            Log.e(TAG, "registerCallback failed", e);
            dvs.unlinkToDeath(deathRecipient, 0);
            scheduleConnect();
            return;
        } finally {
            mRegisteringCallback = false;
        }

        mDvs = dvs;
        mDeathRecipient = deathRecipient;
        mConnectAttempts = 0;
        Log.i(TAG, "Registered with " + DvsClient.SERVICE_NAME);
        mAdjustedBrightnessEnabled =
                mCallback.supportsAdjustedBrightness() && DvsClient.usesAdjustedBrightness();
        Log.i(TAG, "Adjusted brightness feedback enabled: " + mAdjustedBrightnessEnabled);
        if (mPlaybackColorMode != null) {
            mPlaybackColorMode.setEnabled(mAdjustedBrightnessEnabled);
        }
        if (mAdjustedBrightnessEnabled) {
            mBrightnessSettingObserver = new ContentObserver(mHandler) {
                @Override
                public void onChange(boolean selfChange) {
                    if (!selfChange && mPlaying) {
                        pushBrightness();
                        updateAdjustedBrightness(true);
                    }
                }
            };
            getContentResolver().registerContentObserver(
                    Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS),
                    false,
                    mBrightnessSettingObserver);
            updateBrightnessListener();
            updateAdjustedBrightness(true);
        }
    }

    private void scheduleConnect() {
        mHandler.postDelayed(mConnectRunnable, RECONNECT_DELAY_MS);
    }

    private void handleDvsDied(IBinder who) {
        if (who != mDvs) {
            return; // Stale notification from an earlier connection
        }
        Log.w(TAG, "Dolby Vision daemon died, reconnecting");
        mDvs = null;
        mDeathRecipient = null;
        stopAdjustedBrightnessFeedback();
        // The daemon lost its state along with us: playback (if any) is over.
        handlePlaybackStop();
        if (mPlaybackColorMode != null) {
            mPlaybackColorMode.stop(true);
        }
        scheduleConnect();
    }

    // Playback handling

    private void handlePlaybackStart(boolean replayed) {
        if (mDvs == null) {
            return;
        }
        mPlaying = true;
        mLastBrightness = -1;
        if (mPlaybackColorMode != null) {
            mPlaybackColorMode.start(replayed);
        }

        try {
            Log.i(
                    TAG,
                    "Dolby Vision displayIDs: "
                            + DvsClient.getParameters(mDvs, DvsClient.KEY_DISPLAY_IDS));
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "getParameters(displayIDs) failed", e);
        }

        showPqNotification(readPqMode());
        pushBrightness();
        updateBrightnessListener();
        updateAdjustedBrightness(false);
    }

    private void handlePlaybackStop() {
        mPlaying = false;
        if (mPlaybackColorMode != null) {
            mPlaybackColorMode.stop(false);
        }
        mNotificationManager.cancel(PQ_NOTIFICATION_ID);
        updateBrightnessListener();
    }

    private void updateBrightnessListener() {
        final boolean needed = mPlaying || mAdjustedBrightnessEnabled;
        if (needed && !mBrightnessListenerRegistered) {
            long events = DisplayManager.EVENT_TYPE_DISPLAY_BRIGHTNESS;
            if (mAdjustedBrightnessEnabled) {
                events |= DisplayManager.EVENT_TYPE_DISPLAY_ADDED
                        | DisplayManager.EVENT_TYPE_DISPLAY_REMOVED
                        | DisplayManager.EVENT_TYPE_DISPLAY_CHANGED
                        | DisplayManager.EVENT_TYPE_DISPLAY_STATE;
            }
            mDisplayManager.registerDisplayListener(mBrightnessListener, mHandler, events);
            mBrightnessListenerRegistered = true;
        } else if (!needed && mBrightnessListenerRegistered) {
            mDisplayManager.unregisterDisplayListener(mBrightnessListener);
            mBrightnessListenerRegistered = false;
        }
    }

    private void resetAdjustedBrightness() {
        if (mAdjustedBrightnessRamp != null) {
            mAdjustedBrightnessRamp.cancel();
            mAdjustedBrightnessRamp = null;
        }
        mAdjustedBrightnessTarget = Float.NaN;
        mLastAdjustedBrightness = -1;
        mDisplayState = Display.STATE_UNKNOWN;
    }

    private void stopAdjustedBrightnessFeedback() {
        mAdjustedBrightnessEnabled = false;
        resetAdjustedBrightness();
        if (mBrightnessSettingObserver != null) {
            getContentResolver().unregisterContentObserver(mBrightnessSettingObserver);
            mBrightnessSettingObserver = null;
        }
        updateBrightnessListener();
    }

    private void updateAdjustedBrightness(boolean immediate) {
        if (!mAdjustedBrightnessEnabled || mDvs == null) {
            return;
        }
        final Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
        if (display == null || !display.isHdrSdrRatioAvailable()) {
            return;
        }
        final int state = display.getState();
        if (state == Display.STATE_OFF && mDisplayState == state) {
            return;
        }
        final BrightnessInfo info = display.getBrightnessInfo();
        if (info == null || !Float.isFinite(info.adjustedBrightness)
                || info.adjustedBrightness < 0.0f) {
            return;
        }
        final float target = info.adjustedBrightness;
        if (immediate || target != mAdjustedBrightnessTarget) {
            if (mAdjustedBrightnessRamp == null) {
                mAdjustedBrightnessRamp = new AdjustedBrightnessRamp(this::pushAdjustedBrightness);
            }
            final boolean snap = immediate || mDisplayState != state
                    || mLastAdjustedBrightness == (int) (255.0f * target);
            mAdjustedBrightnessRamp.setTarget(target, snap);
            mAdjustedBrightnessTarget = target;
        }
        mDisplayState = state;
    }

    private void pushAdjustedBrightness(float value) {
        if (!mAdjustedBrightnessEnabled || mDvs == null) {
            return;
        }
        final int brightness = (int) Math.min(255.0f, value * 255.0f);
        if (brightness == mLastAdjustedBrightness) {
            return;
        }
        try {
            DvsClient.setAdjustedBrightness(mDvs, brightness);
            mLastAdjustedBrightness = brightness;
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "setParameters(adjustedBrightness) failed", e);
        }
    }

    // Picture mode

    private void handleSetPqMode(int mode) {
        if (mDvs == null || !mPlaying) {
            return;
        }
        if (mode != DvsClient.PQ_MODE_BRIGHT
                && mode != DvsClient.PQ_MODE_VIVID
                && mode != DvsClient.PQ_MODE_DARK) {
            Log.w(TAG, "Ignoring invalid pqMode " + mode);
            return;
        }
        try {
            DvsClient.setPqMode(mDvs, mode);
            Log.i(TAG, "Set pqMode " + mode);
        } catch (RemoteException | RuntimeException e) {
            // A dead daemon is handled by the death recipient.
            Log.w(TAG, "setParameters(pqMode) failed", e);
        }
        // Show what the daemon actually reports, not what was asked for.
        showPqNotification(readPqMode());
    }

    /** Current pqMode from the daemon, or -1 if unknown. The daemon owns the setting. */
    private int readPqMode() {
        try {
            return DvsClient.getPqMode(mDvs);
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "getParameters(pqMode) failed", e);
            return -1;
        }
    }

    private static int pqModeLabel(int mode) {
        switch (mode) {
            case DvsClient.PQ_MODE_BRIGHT:
                return R.string.notification_bright;
            case DvsClient.PQ_MODE_VIVID:
                return R.string.notification_vivid;
            case DvsClient.PQ_MODE_DARK:
                return R.string.notification_dark;
            default:
                return R.string.notification_title;
        }
    }

    private static int pqModeTitle(int mode) {
        switch (mode) {
            case DvsClient.PQ_MODE_BRIGHT:
                return R.string.notification_bright_mode;
            case DvsClient.PQ_MODE_VIVID:
                return R.string.notification_vivid_mode;
            case DvsClient.PQ_MODE_DARK:
                return R.string.notification_dark_mode;
            default:
                return R.string.notification_title;
        }
    }

    private static int pqModeDescription(int mode) {
        switch (mode) {
            case DvsClient.PQ_MODE_BRIGHT:
                return R.string.content_text_bright;
            case DvsClient.PQ_MODE_VIVID:
                return R.string.content_text_vivid;
            case DvsClient.PQ_MODE_DARK:
                return R.string.content_text_dark;
            default:
                return 0;
        }
    }

    /** Posts (or updates) the picture mode notification; {@code current} is -1 if unknown. */
    private void showPqNotification(int current) {
        final Notification.Builder builder =
                new Notification.Builder(this, PQ_CHANNEL_ID)
                        .setSmallIcon(R.drawable.ic_dolby_vision)
                        .setContentTitle(getString(pqModeTitle(current)))
                        .setCategory(Notification.CATEGORY_STATUS)
                        .setOngoing(true)
                        .setOnlyAlertOnce(true);
        final int description = pqModeDescription(current);
        if (description != 0) {
            builder.setContentText(getString(description));
        }
        for (int mode : PQ_MODES) {
            // Stock describes the selected mode and offers only the other modes.
            // Bright/Dark describe viewing environments, not a detected light level.
            if (mode == current) {
                continue;
            }
            final Intent intent =
                    new Intent(ACTION_SET_PQ_MODE)
                            .setPackage(getPackageName())
                            .putExtra(EXTRA_PQ_MODE, mode);
            // The mode is the request code so the PendingIntents stay distinct.
            final PendingIntent pi =
                    PendingIntent.getBroadcast(
                            this,
                            mode,
                            intent,
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            final String label = getString(pqModeLabel(mode));
            builder.addAction(new Notification.Action.Builder(null, label, pi).build());
        }
        mNotificationManager.notify(PQ_NOTIFICATION_ID, builder.build());
    }

    private void pushBrightness() {
        if (mDvs == null) {
            return;
        }
        final int brightness = readBrightness();
        if (brightness < 0 || brightness == mLastBrightness) {
            return;
        }
        try {
            DvsClient.setBrightness(mDvs, brightness);
            mLastBrightness = brightness;
        } catch (RemoteException | RuntimeException e) {
            // A dead daemon is handled by the death recipient.
            Log.w(TAG, "setParameters(brightness) failed", e);
        }
    }

    /** Returns panel brightness on the daemon's 0-255 scale, or -1 if unavailable. */
    private int readBrightness() {
        final Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
        final BrightnessInfo info = display != null ? display.getBrightnessInfo() : null;
        if (info != null && !Float.isNaN(info.brightness)) {
            return clamp(BrightnessSynchronizer.brightnessFloatToInt(info.brightness));
        }
        Log.w(TAG, "No BrightnessInfo, falling back to Settings.System.SCREEN_BRIGHTNESS");
        return clamp(
                Settings.System.getInt(
                        getContentResolver(), Settings.System.SCREEN_BRIGHTNESS, -1));
    }

    private static int clamp(int brightness) {
        return brightness < 0 ? -1 : Math.min(brightness, 255);
    }
}
