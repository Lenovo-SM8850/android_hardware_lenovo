/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.animation.ValueAnimator;
import android.view.Choreographer;

/**
 * Converts display brightness changes into the DVS client's brightness ramp.
 *
 * <p>With {@link DisplayRampRates} the ramp follows the one the display's DisplayPowerController
 * applies to the panel, so the DVS value moves in step with the panel. Without them it falls back
 * to a fixed perceptual ramp.
 */
final class AdjustedBrightnessRamp {
    interface Listener {
        void onBrightness(float brightness);
    }

    // Fixed ramp, used without display ramp rates.
    private static final float RATE = 0.06f;
    private static final float MAX_SECONDS = 3.0f;
    private final Choreographer mChoreographer = Choreographer.getInstance();
    private final Listener mListener;
    private final DisplayRampRates mRates;
    private final Choreographer.FrameCallback mFrame = this::advance;
    private boolean mInitialized;
    private boolean mRunning;
    // Linear brightness 0..1; the ramp itself moves in the space of mLinearSpace.
    private float mCurrent;
    private float mTarget;
    private float mRate;
    private boolean mLinearSpace;
    private long mLastFrame;

    AdjustedBrightnessRamp(Listener listener, DisplayRampRates rates) {
        mListener = listener;
        mRates = rates;
    }

    boolean isRunning() {
        return mRunning;
    }

    /**
     * Ramps to the brightness. The rate is chosen like DisplayPowerController does: slowChange is
     * an auto-brightness change, hdrOnly a change of the HDR boost with the SDR brightness
     * unchanged, which is always fast.
     */
    void setTarget(float brightness, boolean immediate, boolean slowChange, boolean hdrOnly) {
        // DisplayPowerController also jumps to the target while the panel is at zero.
        if (!mInitialized || immediate || (mRates != null && mCurrent == 0.0f)) {
            if (mInitialized && brightness == mCurrent) {
                cancel();
                mTarget = brightness;
                return;
            }
            cancel();
            mInitialized = true;
            mCurrent = mTarget = brightness;
            mListener.onBrightness(brightness);
            return;
        }

        final boolean increasing = brightness > mCurrent;
        final boolean slow = slowChange && !hdrOnly;
        final boolean linear = mRates != null && mRates.linearSpace
                && (increasing || !slow) && !hdrOnly;
        final float delta = Math.abs(toSpace(brightness, linear) - toSpace(mCurrent, linear));
        float rate;
        if (mRates == null) {
            rate = Math.max(RATE, delta / MAX_SECONDS);
        } else {
            if (increasing) {
                rate = slow ? mRates.slowIncrease : mRates.fastIncrease;
            } else {
                rate = slow ? mRates.slowDecrease : mRates.fastDecrease;
            }
            final long maxMillis = increasing ? mRates.increaseMaxMillis : mRates.decreaseMaxMillis;
            if (maxMillis > 0 && delta / rate > maxMillis / 1000.0f) {
                rate = delta / (maxMillis / 1000.0f);
            }
        }
        final boolean reversing = (brightness - mCurrent) * (mTarget - mCurrent) <= 0.0f;
        if (!mRunning || linear != mLinearSpace || rate > mRate || reversing) {
            mRate = rate;
            mLinearSpace = linear;
        }
        mTarget = brightness;
        if (!mRunning && mCurrent != mTarget) {
            mRunning = true;
            mLastFrame = System.nanoTime();
            mChoreographer.postFrameCallback(mFrame);
        }
    }

    void cancel() {
        mChoreographer.removeFrameCallback(mFrame);
        mRunning = false;
    }

    private void advance(long frameTime) {
        final float seconds = (frameTime - mLastFrame) * 1.0e-9f;
        mLastFrame = frameTime;
        final float scale = ValueAnimator.getDurationScale();
        if (scale == 0.0f) {
            mCurrent = mTarget;
        } else {
            final float step = seconds * mRate / scale;
            final float current = toSpace(mCurrent, mLinearSpace);
            final float target = toSpace(mTarget, mLinearSpace);
            final float next = target > current
                    ? Math.min(target, current + step)
                    : Math.max(target, current - step);
            mCurrent = next == target ? mTarget : fromSpace(next, mLinearSpace);
        }
        mListener.onBrightness(Math.max(0.0f, Math.min(mCurrent, 1.0f)));
        mRunning = mCurrent != mTarget;
        if (mRunning) {
            mChoreographer.postFrameCallback(mFrame);
        }
    }

    private static float toSpace(float linear, boolean linearSpace) {
        return linearSpace ? linear : toGamma(linear);
    }

    private static float fromSpace(float value, boolean linearSpace) {
        return linearSpace ? value : toLinear(value);
    }

    // HLG transfer functions, with the float constants used by the DVS client contract.
    private static float toGamma(float linear) {
        final float normalized = 12.0f * linear;
        return normalized <= 1.0f
                ? 0.5f * (float) Math.sqrt(normalized)
                : 0.17883277f * (float) Math.log(normalized - 0.28466892f) + 0.5599107f;
    }

    private static float toLinear(float gamma) {
        final float normalized;
        if (gamma <= 0.5f) {
            final float value = gamma / 0.5f;
            normalized = value * value;
        } else {
            normalized = (float) Math.exp((gamma - 0.5599107f) / 0.17883277f) + 0.28466892f;
        }
        return Math.max(0.0f, Math.min(normalized, 12.0f)) / 12.0f;
    }
}
