/*
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.dolby.dvs;

@VintfStability
interface IDvsCallback {
    boolean onDolbyVisionPlaybackStart() = 0;
    boolean onDolbyVisionPlaybackStop() = 1;
    void onDolbyNotification(in String notification) = 2;
}
