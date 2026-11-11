// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.gamespace

private data class State(val mode: Int, val fi: Int = 0, val sr: Int = 0, val last: Int = 2, val profile: String = "default") {
    fun fi(next: Int) = copy(mode = GppModeRules.modeForChoice(mode, next), fi = next,
        last = GppModeRules.rememberedChoice(last, next))
    fun sr(next: Int) = copy(mode = GppModeRules.modeForChoice(mode, next), sr = next)
    fun profile(next: String) = copy(profile = next).mode(mode)
    fun mode(next: Int) = copy(mode = next,
        fi = if (GppModeRules.keepFeatures(next)) fi else 0,
        sr = if (GppModeRules.keepFeatures(next)) sr else 0)
}
fun main() {
    for (mode in listOf(1, 3)) {
        check(State(mode).fi(1).mode == 2)
        check(State(mode).sr(1).mode == 2)
        // Off notifications from a downgrade must never raise the mode again.
        val lowered = State(2, fi = 2, sr = 1).mode(mode)
        check(lowered.fi == 0 && lowered.sr == 0 && lowered.last == 2)
        check(lowered.fi(0).sr(0).mode == mode)
        check(lowered.fi(lowered.last).mode == 2)
    }
    check(State(2, 2, 1).fi(0).sr(0).mode == 2)
    var selected = State(2, 2, 1)
    for (profile in listOf("user-a", "user-b", "default")) {
        selected = selected.profile(profile)
        check(selected.profile == profile && selected.mode == 2 && selected.fi == 2 && selected.sr == 1)
    }
    check(GppModeRules.modeForChoice(0, 2) == 0)
    println("GPP_MODE_RULES_OK raise off-stays lower-clears remembered profiles-stay no-loop")
}
