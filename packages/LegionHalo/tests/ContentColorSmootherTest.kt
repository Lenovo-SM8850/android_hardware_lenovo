// SPDX-License-Identifier: Apache-2.0
package io.github.miner7222.halo

fun testContentColors() {
    check(!ContentColorSmoother.usable(0))
    check(!ContentColorSmoother.usable(0xffffff))
    check(!ContentColorSmoother.usable(-1))
    check(ContentColorSmoother.usable(0xff0000))
    check(ContentColorSmoother.usable(0x808080))
    val colors = ContentColorSmoother()
    check(colors.value(0x00ff00, 0) == 0x00ff00)
    colors.setTarget(0xff0000, 0)
    colors.setTarget(0x0000ff, 100)
    check(colors.value(0, 100) == 0xff0000)
    val halfway = colors.value(0, 300)
    check(halfway shr 16 in 1..254 && halfway and 255 in 1..254)
    check(colors.moving())
    check(colors.value(0, 4000) == 0x0000ff)
    check(!colors.moving())
    colors.clear()
    check(colors.value(0x00ff00, 4100) == 0x00ff00)
    println("CONTENT_COLOR_SMOOTHING_TESTS_OK")
}
