/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.os.RemoteException;

import vendor.dolby.dvs.IDvsCallback;

/** Receives Dolby Vision playback callbacks without blocking the binder thread. */
final class DvsCallbackBinder extends IDvsCallback.Stub {
    interface Listener {
        void onPlaybackStart();

        void onPlaybackStop();
    }

    // The daemon shares IDvs and callback hashes; the generated superset differs.
    private static final String BALDUR_HASH = "c37499ec236359f61c5c42109200921e04a8fdbc";
    private static final String WUJI_HASH = "9f3d7ea5005e33b60f86f3637fb4bd452d3186c6";
    private volatile String mInterfaceHash = BALDUR_HASH;
    private final Listener mListener;

    DvsCallbackBinder(Listener listener) {
        mListener = listener;
    }

    void configureForService(String hash) throws RemoteException {
        if (!BALDUR_HASH.equals(hash) && !WUJI_HASH.equals(hash)) {
            throw new RemoteException("Unsupported Dolby Vision service interface hash.");
        }
        mInterfaceHash = hash;
    }

    @Override
    public int getInterfaceVersion() {
        return 1;
    }

    @Override
    public String getInterfaceHash() {
        return mInterfaceHash;
    }

    @Override
    public boolean onDolbyVisionPlaybackStart() {
        mListener.onPlaybackStart();
        return true;
    }

    @Override
    public boolean onDolbyVisionPlaybackStop() {
        mListener.onPlaybackStop();
        return true;
    }

    @Override
    public void onDolbyNotification(String notification) {
        // Acknowledge notifications; this app has no notification consumer.
    }
}
