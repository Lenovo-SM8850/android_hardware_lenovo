/*
 * SPDX-License-Identifier: Apache-2.0
 */

package io.github.miner7222.datasim;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.List;

public final class FirstBootPolicyTest {
    @Test
    public void freshDataAdmitsFirstBootOnly() {
        assertTrue(FirstBootPolicy.mayHandle(null, 1));
        assertTrue(FirstBootPolicy.mayHandle(FirstBootPolicy.pending(1), 1));
        assertFalse(FirstBootPolicy.mayHandle(FirstBootPolicy.pending(1), 2));
    }

    @Test
    public void completionPreservesUserChoiceOnEveryBoot() {
        assertFalse(FirstBootPolicy.mayHandle(FirstBootPolicy.COMPLETE, 1));
        assertFalse(FirstBootPolicy.mayHandle(FirstBootPolicy.COMPLETE, 2));
        assertFalse(FirstBootPolicy.mayHandle(FirstBootPolicy.COMPLETE, 100));
    }

    @Test
    public void missingCardDoesNotDeferPolicyToLaterBoot() {
        String state = FirstBootPolicy.pending(1);
        assertEquals(-1, FirstBootPolicy.selectSubscription(1, List.of(), List.of()));
        assertTrue(FirstBootPolicy.mayHandle(state, 1));
        assertFalse(FirstBootPolicy.mayHandle(state, 2));
    }

    @Test
    public void missingBootCountAndUnknownStateFailClosed() {
        assertFalse(FirstBootPolicy.mayHandle(null, -1));
        assertFalse(FirstBootPolicy.mayHandle("invalid", 1));
    }

    @Test
    public void physicalIdentitySelectsArbitrarySubscriptionId() {
        List<FirstBootPolicy.Card> cards = List.of(card(0, "89001"), card(1, "89002"));
        // No dependence on logical slot, subscription ordering, or subId 2.
        List<FirstBootPolicy.Subscription> subscriptions =
                List.of(subscription(42, "89002"), subscription(2, "89001"));
        assertEquals(42, FirstBootPolicy.selectSubscription(1, cards, subscriptions));
        assertEquals(2, FirstBootPolicy.selectSubscription(0, cards, subscriptions));
        assertEquals(-1, FirstBootPolicy.selectSubscription(-1, cards, subscriptions));
    }

    @Test
    public void absentOrUnavailableCardNeverSelectsUserSim() {
        List<FirstBootPolicy.Subscription> subscriptions = List.of(subscription(42, "89002"));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(1, List.of(card(0, "89002")), subscriptions));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(
                        1, List.of(new FirstBootPolicy.Card(1, false, "89002")), subscriptions));
        assertEquals(
                -1, FirstBootPolicy.selectSubscription(1, List.of(card(1, null)), subscriptions));
        assertEquals(
                -1, FirstBootPolicy.selectSubscription(1, List.of(card(1, "")), subscriptions));
    }

    @Test
    public void waitsForMatchingSubscriptionInFirstSession() {
        List<FirstBootPolicy.Card> cards = List.of(card(1, "89002"));
        assertEquals(-1, FirstBootPolicy.selectSubscription(1, cards, List.of()));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(1, cards, List.of(subscription(42, "89001"))));
        assertEquals(
                42,
                FirstBootPolicy.selectSubscription(1, cards, List.of(subscription(42, "89002"))));
    }

    @Test
    public void paddingIsNormalizedButRedactedAndAmbiguousIdsAreRejected() {
        assertEquals(
                42,
                FirstBootPolicy.selectSubscription(
                        1, List.of(card(1, "89002FF")), List.of(subscription(42, "89002f"))));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(
                        1, List.of(card(1, "FFFF")), List.of(subscription(42, ""))));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(
                        1,
                        List.of(card(1, "89002")),
                        List.of(subscription(42, "89002"), subscription(43, "89002"))));
        assertEquals(
                -1,
                FirstBootPolicy.selectSubscription(
                        1, List.of(card(1, "89002")), List.of(subscription(-1, "89002"))));
    }

    private static FirstBootPolicy.Card card(int slot, String iccid) {
        return new FirstBootPolicy.Card(slot, true, iccid);
    }

    private static FirstBootPolicy.Subscription subscription(int id, String iccid) {
        return new FirstBootPolicy.Subscription(id, iccid);
    }
}
