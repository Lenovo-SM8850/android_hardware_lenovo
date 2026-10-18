/* SPDX-License-Identifier: Apache-2.0 */
package io.github.miner7222.pen

import android.bluetooth.BluetoothGattCharacteristic
import android.view.MotionEvent
import java.util.UUID

internal object PenHapticProtocol {
    const val SETTING = "pen_writing_haptics_enabled"
    const val VID = 0x17ef
    const val PID = 0x622e
    val SERVICE = uuid("00000000")
    val CONTINUOUS = uuid("00000006")
    val SWITCH = uuid("0000000e")
    val REQUEST = uuid("00000002")
    val INFO = uuid("0000000a")
    val TIMING_SERVICE = UUID.fromString("0000fe40-cc7a-482a-984a-7f2ed5b3e512")
    val TIMING = UUID.fromString("0000fe41-cc7a-482a-984a-7f2ed5b3e512")
    val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    fun stop() = byteArrayOf(0, 0, 0, 0)
    fun contact(tool: Int): ByteArray = when (tool) {
        MotionEvent.TOOL_TYPE_STYLUS -> byteArrayOf(0x21, 3, 1, 1)
        MotionEvent.TOOL_TYPE_ERASER -> byteArrayOf(0x23, 3, 1, 0)
        else -> throw IllegalArgumentException("Unsupported haptic tool")
    }
    fun writeType(c: BluetoothGattCharacteristic): Int? = when {
        c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ->
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0 ->
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        else -> null
    }
    private fun uuid(prefix: String) = UUID.fromString("$prefix-000f-11e1-9ab4-0002a5d5c51b")
}
