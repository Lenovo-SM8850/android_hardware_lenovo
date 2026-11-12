/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touch;

import android.app.Application;
import android.os.Handler;
import android.os.HandlerThread;

/** Runs the display rotation writer and the game touch policy in one persistent process. */
public final class TouchApplication extends Application {
    private TouchRotationController mRotation;
    private TouchPolicyController mPolicy;

    @Override
    public void onCreate() {
        super.onCreate();
        HandlerThread thread = new HandlerThread("LenovoTouch");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        mPolicy = new TouchPolicyController(this, handler);
        mRotation = new TouchRotationController(this, handler, mPolicy::onRotationChanged);
        mRotation.start();
        mPolicy.start();
    }
}
