/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.dolbyvision;

import android.content.Context;
import android.content.res.Resources;
import android.os.Environment;
import android.util.Log;
import android.util.Xml;
import android.view.Display;
import android.view.DisplayAddress;

import com.android.internal.display.BrightnessSynchronizer;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The brightness ramp rates the display's DisplayPowerController applies to the panel.
 *
 * <p>The rates come from the per-display config file, chosen like DisplayDeviceConfig does
 * (product then vendor directory; stable id, plain id, port), and from the framework ramp rate
 * integers when no file provides all four rates.
 */
final class DisplayRampRates {
    private static final String TAG = DolbyVisionService.TAG;

    private static final long STABLE_FLAG = 1L << 62;
    private static final String DISPLAY_CONFIG_DIR = "displayconfig";
    private static final String LOCAL_PREFIX = "local:";
    private static final String ROOT_ELEMENT = "displayConfiguration";

    final float fastIncrease;
    final float fastDecrease;
    final float slowIncrease;
    final float slowDecrease;
    /** Longest ramp in milliseconds, 0 when unlimited. */
    final long increaseMaxMillis;
    final long decreaseMaxMillis;
    /** The panel ramps in linear space (not HLG gamma) when it is not a slow decrease. */
    final boolean linearSpace;
    final String source;

    private DisplayRampRates(float fastIncrease, float fastDecrease, float slowIncrease,
            float slowDecrease, long increaseMaxMillis, long decreaseMaxMillis,
            boolean linearSpace, String source) {
        this.fastIncrease = fastIncrease;
        this.fastDecrease = fastDecrease;
        this.slowIncrease = slowIncrease;
        this.slowDecrease = slowDecrease;
        this.increaseMaxMillis = increaseMaxMillis;
        this.decreaseMaxMillis = decreaseMaxMillis;
        this.linearSpace = linearSpace;
        this.source = source;
    }

    /** Returns the rates of the display, or null when they cannot be determined. */
    static DisplayRampRates load(Context context, Display display) {
        final Resources resources = context.getResources();
        final boolean linearSpace;
        float fastIncrease;
        float fastDecrease;
        float slowIncrease;
        float slowDecrease;
        try {
            linearSpace = resources.getBoolean(
                    com.android.internal.R.bool.config_brightnessRampLinearSpace);
            // loadBrightnessRampsFromConfigXml(): one pair of values for both directions.
            fastIncrease = BrightnessSynchronizer.brightnessIntToFloat(resources.getInteger(
                    com.android.internal.R.integer.config_brightness_ramp_rate_fast));
            slowIncrease = BrightnessSynchronizer.brightnessIntToFloat(resources.getInteger(
                    com.android.internal.R.integer.config_brightness_ramp_rate_slow));
        } catch (RuntimeException e) {
            Log.w(TAG, "Framework brightness ramp config unavailable", e);
            return null;
        }
        fastDecrease = fastIncrease;
        slowDecrease = slowIncrease;
        long increaseMax = 0;
        long decreaseMax = 0;
        String source = "framework config";

        final File file = findConfigFile(display);
        if (file != null) {
            try {
                final float[] values = new float[4];
                final long[] maxMillis = new long[2];
                if (readConfigFile(file, values, maxMillis)) {
                    fastDecrease = values[0];
                    fastIncrease = values[1];
                    slowDecrease = values[2];
                    slowIncrease = values[3];
                    source = file.getPath();
                } else {
                    Log.w(TAG, file + " lacks some of the four ramp rates, using framework config");
                }
                increaseMax = maxMillis[0];
                decreaseMax = maxMillis[1];
            } catch (IOException | XmlPullParserException | RuntimeException e) {
                Log.w(TAG, "Cannot read brightness ramps from " + file
                        + ", using framework config", e);
            }
        }
        if (!(fastIncrease > 0.0f) || !(fastDecrease > 0.0f)
                || !(slowIncrease > 0.0f) || !(slowDecrease > 0.0f)) {
            Log.w(TAG, "Invalid brightness ramp rates from " + source);
            return null;
        }
        return new DisplayRampRates(fastIncrease, fastDecrease, slowIncrease, slowDecrease,
                increaseMax, decreaseMax, linearSpace, source);
    }

    @Override
    public String toString() {
        return "fastIncrease=" + fastIncrease + " fastDecrease=" + fastDecrease
                + " slowIncrease=" + slowIncrease + " slowDecrease=" + slowDecrease
                + " increaseMaxMillis=" + increaseMaxMillis
                + " decreaseMaxMillis=" + decreaseMaxMillis
                + " space=" + (linearSpace ? "linear" : "gamma") + " source=" + source;
    }

    private static File findConfigFile(Display display) {
        // Apps get the display without its address; its unique ID of a built-in panel,
        // "local:<physical display id>", names the same file.
        final DisplayAddress address = display.getAddress();
        final long id;
        int port = -1;
        if (address instanceof DisplayAddress.Physical) {
            final DisplayAddress.Physical physical = (DisplayAddress.Physical) address;
            id = physical.getPhysicalDisplayId();
            port = physical.getPort();
        } else {
            final String uniqueId = display.getUniqueId();
            if (uniqueId == null || !uniqueId.startsWith(LOCAL_PREFIX)) {
                Log.i(TAG, "Display has no physical ID, no display config file");
                return null;
            }
            try {
                id = Long.parseUnsignedLong(uniqueId.substring(LOCAL_PREFIX.length()));
            } catch (NumberFormatException e) {
                Log.i(TAG, "Display has no physical ID, no display config file");
                return null;
            }
        }
        final List<String> names = new ArrayList<>();
        names.add(String.format(Locale.ROOT, "display_id_%d.xml", id));
        names.add(String.format(Locale.ROOT, "display_%d.xml", id & ~STABLE_FLAG));
        if (port >= 0) {
            names.add(String.format(Locale.ROOT, "display_port_%d.xml", port));
        }
        for (File base : new File[] {
            Environment.getProductDirectory(), Environment.getVendorDirectory()
        }) {
            for (String name : names) {
                final File file = Environment.buildPath(base, "etc", DISPLAY_CONFIG_DIR, name);
                if (file.isFile()) {
                    return file;
                }
            }
        }
        Log.i(TAG, "No readable display config file for " + names);
        return null;
    }

    /**
     * Reads fast/slow decrease/increase into values (that order) and the increase/decrease max
     * millis into maxMillis. Returns false unless all four rates are present.
     */
    private static boolean readConfigFile(File file, float[] values, long[] maxMillis)
            throws IOException, XmlPullParserException {
        final String[] names = {
            "screenBrightnessRampFastDecrease", "screenBrightnessRampFastIncrease",
            "screenBrightnessRampSlowDecrease", "screenBrightnessRampSlowIncrease"
        };
        final boolean[] found = new boolean[names.length];
        try (InputStream in = new BufferedInputStream(new FileInputStream(file))) {
            final XmlPullParser parser = Xml.newPullParser();
            parser.setInput(in, null);
            int type;
            while ((type = parser.next()) != XmlPullParser.END_DOCUMENT) {
                if (type != XmlPullParser.START_TAG) {
                    continue;
                }
                if (parser.getDepth() == 1 && !ROOT_ELEMENT.equals(parser.getName())) {
                    throw new XmlPullParserException("Unexpected root " + parser.getName());
                }
                if (parser.getDepth() != 2) {
                    continue;
                }
                final String name = parser.getName();
                for (int i = 0; i < names.length; i++) {
                    if (names[i].equals(name)) {
                        values[i] = Float.parseFloat(parser.nextText().trim());
                        found[i] = true;
                    }
                }
                if ("screenBrightnessRampIncreaseMaxMillis".equals(name)) {
                    maxMillis[0] = Long.parseLong(parser.nextText().trim());
                } else if ("screenBrightnessRampDecreaseMaxMillis".equals(name)) {
                    maxMillis[1] = Long.parseLong(parser.nextText().trim());
                }
            }
        }
        for (boolean present : found) {
            if (!present) {
                return false;
            }
        }
        return true;
    }
}
