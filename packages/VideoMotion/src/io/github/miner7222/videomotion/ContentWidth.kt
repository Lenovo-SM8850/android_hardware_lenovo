// SPDX-License-Identifier: Apache-2.0

package io.github.miner7222.videomotion

import android.content.res.Configuration
import android.view.View
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity

/** Keeps a single-panel page to a readable column that is centred on wide windows. */
internal object ContentWidth {
    private const val MAX_WIDTH_DP = 840
    private const val TALL_WINDOW_DP = 480

    /** Space on each side of the column for a view that is [View.getWidth] wide. */
    private fun sideInset(view: View): Int {
        val max = Math.round(MAX_WIDTH_DP * view.resources.displayMetrics.density)
        return maxOf((view.width - max) / 2, 0)
    }

    /** Pads the scrolling list so its rows stay inside the column while the list scrolls full width. */
    fun constrain(list: View) {
        val start = list.paddingStart
        val end = list.paddingEnd
        var applied = 0
        list.addOnLayoutChangeListener { view, _, _, _, _, _, _, _, _ ->
            val inset = sideInset(view)
            if (inset != applied) {
                applied = inset
                view.setPaddingRelative(start + inset, view.paddingTop, end + inset, view.paddingBottom)
            }
        }
    }

    /**
     * Shows the large title in landscape as well, and starts it where the column starts so title,
     * intro text and rows share one edge in both orientations.
     */
    fun alignTitle(activity: CollapsingToolbarBaseActivity, expandInLandscape: Boolean) {
        val toolbar = activity.collapsingToolbarLayout ?: return
        val frame = activity.findViewById<View>(com.android.settingslib.collapsingtoolbar.R.id.content_frame)
        val start = toolbar.expandedTitleMarginStart
        val end = toolbar.expandedTitleMarginEnd
        var applied = 0
        frame.addOnLayoutChangeListener { view, _, _, _, _, _, _, _, _ ->
            val inset = sideInset(view)
            if (inset != applied) {
                applied = inset
                toolbar.expandedTitleMarginStart = start + inset
                toolbar.expandedTitleMarginEnd = end + inset
            }
        }
        val configuration = activity.resources.configuration
        if (expandInLandscape && configuration.orientation == Configuration.ORIENTATION_LANDSCAPE &&
            configuration.screenHeightDp >= TALL_WINDOW_DP) {
            activity.appBarLayout?.let { bar -> bar.post { bar.setExpanded(true, false) } }
        }
    }
}
