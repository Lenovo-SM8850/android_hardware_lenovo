/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.animation.ValueAnimator;
import android.view.Choreographer;

/** Converts display brightness changes into the DVS client's perceptual brightness ramp. */
final class AdjustedBrightnessRamp {
    interface Listener {
        void onBrightness(float brightness);
    }

    private static final float RATE = 0.06f;
    private static final float MAX_SECONDS = 3.0f;
    private final Choreographer mChoreographer = Choreographer.getInstance();
    private final Listener mListener;
    private final Choreographer.FrameCallback mFrame = this::advance;
    private boolean mInitialized;
    private boolean mRunning;
    private float mCurrent;
    private float mTarget;
    private float mRate;
    private long mLastFrame;

    AdjustedBrightnessRamp(Listener listener) {
        mListener = listener;
    }

    void setTarget(float brightness, boolean immediate) {
        final float target = toGamma(brightness);
        if (!mInitialized || immediate) {
            if (mInitialized && target == mCurrent) {
                cancel();
                mTarget = target;
                return;
            }
            cancel();
            mInitialized = true;
            mCurrent = mTarget = target;
            mListener.onBrightness(brightness);
            return;
        }

        final float rate = Math.max(RATE, Math.abs(target - mCurrent) / MAX_SECONDS);
        final boolean reversing = (target - mCurrent) * (mTarget - mCurrent) <= 0.0f;
        if (!mRunning || rate > mRate || reversing) {
            mRate = rate;
        }
        mTarget = target;
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
            mCurrent = mTarget > mCurrent
                    ? Math.min(mTarget, mCurrent + step)
                    : Math.max(mTarget, mCurrent - step);
        }
        mListener.onBrightness(toLinear(mCurrent));
        mRunning = mCurrent != mTarget;
        if (mRunning) {
            mChoreographer.postFrameCallback(mFrame);
        }
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
