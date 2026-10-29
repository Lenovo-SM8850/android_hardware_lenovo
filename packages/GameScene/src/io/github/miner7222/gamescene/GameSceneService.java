// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.gamescene;

import android.app.IGameManagerService;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.Log;
import android.util.SparseArray;

import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.regex.Pattern;

/** Receives performance SDK scene reports for the verified active game session. */
public final class GameSceneService extends Service {
    private static final String TAG = "GameScene";
    // Generic SDK interoperability constants; this service uses its own package identity.
    private static final String DESCRIPTOR = "com.ihoc.mgpa.vendorsdk.v2_0.ITGPAServer";
    private static final String STRATEGY = "{\"Version\":\"2.0.1\",\"IsSupport\":1,"
            + "\"SupportFunc\":\"1,2,7\",\"SupportScene\":\"3,4,7\"}";
    private static final int MAX_JSON_BYTES = 16 * 1024;
    private static final int MAX_CLIENTS = 64;
    private static final Pattern KEY = Pattern.compile("[0-9]{1,10}");
    private static final Pattern SCENE = Pattern.compile("[0-9]{1,6}");
    private final SparseArray<Client> mClients = new SparseArray<>(); // Calling PID.
    private final SparseArray<Rate> mRates = new SparseArray<>(); // Shared by each UID's clients.
    private volatile IGameManagerService mGameManager;

    private static final class Rate {
        final long[] times = new long[20];
        int next;
        int size;

        boolean recent() {
            return size > 0 && SystemClock.elapsedRealtime()
                    - times[(next + times.length - 1) % times.length] < 1000;
        }

        boolean admit() {
            final long now = SystemClock.elapsedRealtime();
            while (size > 0 && now - times[(next - size + times.length) % times.length] >= 1000) {
                size--;
            }
            if (size == times.length) return false;
            times[next] = now;
            next = (next + 1) % times.length;
            size++;
            return true;
        }
    }

    private final class Client implements IBinder.DeathRecipient {
        final int uid;
        final int pid;
        final IBinder token;
        final Rate rate;

        Client(int uid, int pid, IBinder token, Rate rate) {
            this.uid = uid;
            this.pid = pid;
            this.token = token;
            this.rate = rate;
        }

        @Override
        public void binderDied() {
            synchronized (mClients) {
                if (mClients.get(pid) == this) removeClient(this);
            }
        }
    }

    private void removeClient(Client client) {
        mClients.remove(client.pid);
        client.token.unlinkToDeath(client, 0);
        // Keep a retired UID's budget until the rate window ends.
        if (!hasClient(client.uid) && !client.rate.recent()) mRates.remove(client.uid);
    }

    private boolean hasClient(int uid) {
        for (int i = 0; i < mClients.size(); i++) {
            if (mClients.valueAt(i).uid == uid) return true;
        }
        return false;
    }

    private void pruneRates() {
        for (int i = mRates.size() - 1; i >= 0; i--) {
            if (!mRates.valueAt(i).recent() && !hasClient(mRates.keyAt(i))) mRates.removeAt(i);
        }
    }

    private void init(int uid, int pid, IBinder token) throws RemoteException {
        if (token == null || pid <= 0) return;
        synchronized (mClients) {
            pruneRates();
            final Client prior = mClients.get(pid);
            if (prior != null && prior.uid == uid && prior.token.equals(token)) return;
            // Preserve the UID's rate history when a process replaces its client token.
            Rate rate = mRates.get(uid);
            if (prior != null) removeClient(prior);
            if (mClients.size() >= MAX_CLIENTS
                    || (rate == null && mRates.size() >= 2 * MAX_CLIENTS)) return;
            if (rate == null) rate = new Rate();
            final Client client = new Client(uid, pid, token, rate);
            token.linkToDeath(client, 0);
            mRates.put(uid, rate);
            mClients.put(pid, client);
        }
    }

    @Override
    public void onDestroy() {
        synchronized (mClients) {
            while (mClients.size() > 0) removeClient(mClients.valueAt(0));
            mRates.clear();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }

    private final Binder mBinder = new Binder() {
        {
            attachInterface(null, DESCRIPTOR);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == INTERFACE_TRANSACTION) {
                reply.writeString(DESCRIPTOR);
                return true;
            }
            if (code < 1 || code > 10) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            final int uid = Binder.getCallingUid();
            final int pid = Binder.getCallingPid();
            switch (code) {
                case 1:
                    reply.writeNoException();
                    reply.writeString("v1");
                    break;
                case 2:
                    init(uid, pid, data.readStrongBinder());
                    reply.writeNoException();
                    break;
                case 3:
                    reply.writeNoException();
                    reply.writeInt(0);
                    break;
                case 4:
                    data.readInt(); // Foreground claims do not establish a policy session.
                    reply.writeNoException();
                    break;
                case 5:
                    reply.writeNoException();
                    reply.writeString("2.0.1");
                    break;
                case 6:
                    reply.writeNoException();
                    reply.writeString(STRATEGY);
                    break;
                case 7:
                    data.readStrongBinder(); // Never invoke a game's callback binder.
                    reply.writeNoException();
                    break;
                case 8:
                    boolean admitted;
                    synchronized (mClients) {
                        final Client client = mClients.get(pid);
                        admitted = client != null && client.uid == uid && client.rate.admit();
                    }
                    // Bound the Parcel before allocating its string, then enforce the UTF-8 cap.
                    if (admitted && data.dataAvail() <= 2 * MAX_JSON_BYTES + 8) {
                        report(uid, pid, data.readString());
                    }
                    reply.writeNoException();
                    break;
                case 9:
                    data.readInt();
                    data.readString();
                    reply.writeNoException();
                    reply.writeString(null);
                    break;
                case 10:
                    reply.writeNoException();
                    break;
                default:
                    throw new AssertionError(code);
            }
            return true;
        }
    };

    private void report(int uid, int pid, String json) {
        if (json == null || json.length() > MAX_JSON_BYTES
                || json.getBytes(StandardCharsets.UTF_8).length > MAX_JSON_BYTES) return;
        final int scene;
        try {
            final JSONObject object = new JSONObject(json);
            if (Log.isLoggable(TAG, Log.DEBUG)) {
                final StringBuilder names = new StringBuilder("Report keys:");
                final Iterator<String> keys = object.keys();
                while (keys.hasNext()) {
                    final String key = keys.next();
                    // Numeric SDK key names only; arbitrary names may contain identifiers.
                    if (KEY.matcher(key).matches()) names.append(' ').append(key);
                }
                Log.d(TAG, names.toString());
            }
            final Object value = object.opt("4");
            if (!(value instanceof String) && !(value instanceof Integer)
                    && !(value instanceof Long)) return;
            final String text = value.toString();
            if (!SCENE.matcher(text).matches()) return;
            scene = Integer.parseInt(text);
            if (scene > 100000) return;
        } catch (JSONException | NumberFormatException e) {
            return; // Exception messages can contain private payload values.
        }
        final long identity = Binder.clearCallingIdentity();
        try {
            IGameManagerService manager = mGameManager;
            if (manager == null) {
                manager = IGameManagerService.Stub.asInterface(
                        ServiceManager.getService(Context.GAME_SERVICE));
                mGameManager = manager;
            }
            if (manager != null) manager.reportGameScene(uid, pid, scene);
        } catch (RemoteException e) {
            mGameManager = null;
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }
}
