/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.datasim;

import java.util.List;

/** Pure first-boot admission and physical-card selection logic. */
final class FirstBootPolicy {
    static final String COMPLETE = "complete";
    static final int NO_SUBSCRIPTION = -1;

    private FirstBootPolicy() {}

    // One stored key: absent = fresh, boot count = pending in that boot, complete = finished.
    // A pending stamp prevents an absent card or an abrupt reboot from deferring the policy.
    static boolean mayHandle(String done, int bootCount) {
        return bootCount >= 0 && (done == null || done.equals(pending(bootCount)));
    }

    static String pending(int bootCount) {
        return Integer.toString(bootCount);
    }

    static final class Card {
        final int physicalSlot;
        final boolean present;
        final String iccid;

        Card(int physicalSlot, boolean present, String iccid) {
            this.physicalSlot = physicalSlot;
            this.present = present;
            this.iccid = iccid;
        }
    }

    static final class Subscription {
        final int id;
        final String iccid;

        Subscription(int id, String iccid) {
            this.id = id;
            this.iccid = iccid;
        }
    }

    static int selectSubscription(
            int physicalSlot, List<Card> cards, List<Subscription> subscriptions) {
        if (physicalSlot < 0) {
            return NO_SUBSCRIPTION;
        }
        String iccid = "";
        for (Card card : cards) {
            if (card.physicalSlot == physicalSlot && card.present) {
                iccid = normalizeIccid(card.iccid);
                break;
            }
        }
        if (iccid.isEmpty()) {
            return NO_SUBSCRIPTION;
        }
        int selected = NO_SUBSCRIPTION;
        for (Subscription subscription : subscriptions) {
            if (subscription.id >= 0 && iccid.equals(normalizeIccid(subscription.iccid))) {
                if (selected != NO_SUBSCRIPTION) {
                    return NO_SUBSCRIPTION;
                }
                selected = subscription.id;
            }
        }
        return selected;
    }

    private static String normalizeIccid(String value) {
        if (value == null) {
            return "";
        }
        int end = value.length();
        while (end > 0 && (value.charAt(end - 1) == 'F' || value.charAt(end - 1) == 'f')) {
            end--;
        }
        for (int i = 0; i < end; i++) {
            if (value.charAt(i) < '0' || value.charAt(i) > '9') {
                return "";
            }
        }
        return value.substring(0, end);
    }
}
