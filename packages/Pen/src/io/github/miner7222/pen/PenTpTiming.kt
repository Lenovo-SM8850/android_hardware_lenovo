/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.os.SystemProperties
import android.util.Log
import java.io.File

/** Readback proves host submission only, not TSA or motor acknowledgement. */
internal class PenTpTiming {
    data class State(val generation: Long, val timing: String)

    fun request(enabled: Boolean) {
        try {
            SystemProperties.set("sys.lenovo.pen.haptics", if (enabled) "1" else "0")
        } catch (e: RuntimeException) {
            Log.w(TAG, "Cannot request TP haptics", e)
        }
    }

    fun read(): State? = try {
        val fields = File("/proc/haptics").readText().trim().split(' ')
            .mapNotNull { it.split('=', limit = 2).takeIf { pair -> pair.size == 2 }
                ?.let { pair -> pair[0] to pair[1] } }.toMap()
        if (fields["desired"] == "1" && fields["submitted"] == "1" &&
            fields["status"] == "0") {
            val timing = fields["timing"]?.takeIf { it.matches(Regex("[0-9A-Fa-f]{2}")) }
            val generation = fields["generation"]?.toLongOrNull()
            if (timing != null && generation != null) State(generation, timing) else null
        } else null
    } catch (e: Exception) {
        Log.w(TAG, "Cannot read TP haptic host status", e)
        null
    }

    companion object { private const val TAG = "PenTpTiming" }
}
