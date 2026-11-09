/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.touch;

import android.app.Application;

public final class TouchApplication extends Application {
    private TouchRotationController mRotation;

    @Override
    public void onCreate() {
        super.onCreate();
        mRotation = new TouchRotationController(this);
        mRotation.start();
    }
}
