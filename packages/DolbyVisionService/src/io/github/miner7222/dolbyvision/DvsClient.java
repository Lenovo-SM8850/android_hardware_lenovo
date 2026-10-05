/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.os.IBinder;
import android.os.RemoteException;
import android.view.Display;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;

import vendor.dolby.dvs.IDvs;

/** Binder client for vendor.dolby.dvs.IDvs version 1. */
final class DvsClient {
    /** ServiceManager name of the daemon (declared in the vendor VINTF manifest). */
    static final String SERVICE_NAME = "vendor.dolby.dvs.IDvs/default";

    static final String DESCRIPTOR = "vendor.dolby.dvs.IDvs";

    // Parameter keys, sent as "key=value;key=value".
    static final String KEY_BRIGHTNESS = "brightness"; // int 0-255
    static final String KEY_ADJUSTED_BRIGHTNESS = "adjustedBrightness"; // int 0-255
    static final String KEY_DISPLAY_IDS = "displayIDs"; // Read only
    static final String KEY_PQ_MODE = "pqMode"; // int, see PQ_MODE_*

    // Picture quality modes, as in the daemon's dolby_vision.cfg.
    static final int PQ_MODE_BRIGHT = 0;
    static final int PQ_MODE_VIVID = 1;
    static final int PQ_MODE_DARK = 2;

    private DvsClient() {}

    private static IDvs asInterface(IBinder binder) throws RemoteException {
        IDvs service = IDvs.Stub.asInterface(binder);
        if (service == null) {
            throw new RemoteException("Dolby Vision service is unavailable.");
        }
        return service;
    }

    static void registerCallback(IBinder dvs, IBinder callback) throws RemoteException {
        IDvs service = asInterface(dvs);
        if (callback instanceof DvsCallbackBinder) {
            ((DvsCallbackBinder) callback).configureForService(service.getInterfaceHash());
        }
        service.registerCallback(vendor.dolby.dvs.IDvsCallback.Stub.asInterface(callback));
    }

    static void setParameters(IBinder dvs, String params) throws RemoteException {
        asInterface(dvs).setParameters(params);
    }

    static String getParameters(IBinder dvs, String keys) throws RemoteException {
        return asInterface(dvs).getParameters(keys);
    }

    static void setBrightness(IBinder dvs, int brightness) throws RemoteException {
        setParameters(dvs, KEY_BRIGHTNESS + "=" + brightness);
    }

    static void setAdjustedBrightness(IBinder dvs, int brightness) throws RemoteException {
        setParameters(dvs, KEY_ADJUSTED_BRIGHTNESS + "=" + brightness);
    }

    /** Follows the daemon's config search order; only display 0 with BLUMode=1 opts in. */
    static boolean usesAdjustedBrightness() {
        final String[] paths = {
            "/odm/etc/dolby/dolby_vision_clstc.cfg",
            "/odm/etc/dolby/display/dolby_vision.cfg",
            "/odm/vendor/persist/display/dolby_vision.cfg",
            "/vendor/persist/display/dolby_vision.cfg",
            "/vendor/etc/dolby_vision.cfg"
        };
        for (String path : paths) {
            try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
                boolean displaySection = false;
                int displayId = -1;
                int mode = -1;
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.split("#", 2)[0].trim();
                    if (line.startsWith("[")) {
                        if (displaySection && displayId == Display.DEFAULT_DISPLAY) {
                            return mode == 1;
                        }
                        displaySection = "[Display]".equals(line);
                        displayId = -1;
                        mode = -1;
                    } else if (displaySection) {
                        final String[] pair = line.split("=", 2);
                        if (pair.length == 2) {
                            if ("ID".equals(pair[0].trim())) {
                                displayId = Integer.parseInt(pair[1].trim());
                            } else if ("BLUMode".equals(pair[0].trim())) {
                                mode = Integer.parseInt(pair[1].trim());
                            }
                        }
                    }
                }
                return displaySection && displayId == Display.DEFAULT_DISPLAY && mode == 1;
            } catch (IOException e) {
                // Try the next config when this path is absent or unreadable.
            } catch (NumberFormatException e) {
                return false;
            }
        }
        return false;
    }

    static void setPqMode(IBinder dvs, int mode) throws RemoteException {
        setParameters(dvs, KEY_PQ_MODE + "=" + mode);
    }

    /** Current picture mode, or -1 if the daemon did not report one or it cannot be parsed. */
    static int getPqMode(IBinder dvs) throws RemoteException {
        final String reply = getParameters(dvs, KEY_PQ_MODE);
        if (reply == null) {
            return -1;
        }
        for (String pair : reply.split(";")) {
            final int eq = pair.indexOf('=');
            if (eq > 0 && KEY_PQ_MODE.equals(pair.substring(0, eq).trim())) {
                try {
                    return Integer.parseInt(pair.substring(eq + 1).trim());
                } catch (NumberFormatException e) {
                    return -1;
                }
            }
        }
        return -1;
    }
}
