/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touch;

import android.app.Application;

/** Runs the display rotation writer and the game touch policy in one persistent process. */
public final class TouchApplication extends Application {
    private TouchRotationController mRotation;
    private TouchPolicyController mPolicy;

    @Override
    public void onCreate() {
        super.onCreate();
        mRotation = new TouchRotationController(this);
        mRotation.start();
        mPolicy = new TouchPolicyController(this);
        mPolicy.start();
    }
}
