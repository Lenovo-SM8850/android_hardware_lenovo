# SPDX-License-Identifier: Apache-2.0
"""Small deterministic Android substitutes for exercising production worker lifecycles."""
STUBS = {
    'android/os/Handler.kt': '''package android.os
object SystemClock {
    var now = 0L
    fun uptimeMillis() = now
    fun elapsedRealtime() = now
}
class Handler {
    private val queue = mutableListOf<Pair<Long, Runnable>>()
    fun post(r: Runnable): Boolean = postAtTime(r, SystemClock.now)
    fun postDelayed(r: Runnable, delay: Long): Boolean = postAtTime(r, SystemClock.now + delay)
    fun postAtTime(r: Runnable, time: Long): Boolean { queue.add(time to r); return true }
    fun removeCallbacks(r: Runnable) { queue.removeAll { it.second === r } }
    fun advance(ms: Long) {
        val end = SystemClock.now + ms
        var turns = 0
        while (true) {
            val next = queue.minByOrNull { it.first } ?: break
            if (next.first > end) break
            check(++turns < 10000) { "Unbounded callback loop" }
            queue.remove(next)
            SystemClock.now = next.first
            next.second.run()
        }
        SystemClock.now = end
    }
}
object UserHandle {
    const val USER_SYSTEM = 0
    fun getUserId(uid: Int) = uid / 100000
}
''',
    'android/util/Log.kt': '''package android.util
object Log {
    fun i(tag: String, message: String) = 0
    fun w(tag: String, message: String, error: Exception? = null) = 0
}
''',
    'android/media/audiofx/Visualizer.kt': '''package android.media.audiofx
class Visualizer(session: Int) {
    var captureSize = 1024
        private set
    val samplingRate = 48000000
    init { attempts++; if (createFails) throw IllegalStateException("Transient create failure") }
    fun setEnabled(enabled: Boolean) = SUCCESS
    fun setCaptureSize(size: Int): Int { captureSize = size; return SUCCESS }
    fun setScalingMode(mode: Int) = SUCCESS
    fun getFft(bytes: ByteArray): Int {
        if (captureFails) return -7
        bytes.fill(0)
        if (!silent) { bytes[2] = 40; bytes[6] = 20 }
        return SUCCESS
    }
    fun release() { releases++ }
    companion object {
        const val SUCCESS = 0
        const val SCALING_MODE_NORMALIZED = 0
        var createFails = false
        var captureFails = false
        var silent = false
        var attempts = 0
        var releases = 0
        fun getCaptureSizeRange() = intArrayOf(128, 1024)
    }
}
''',
    'android/media/AudioManager.kt': '''package android.media
import android.os.Handler
class AudioAttributes(val usage: Int) {
    companion object { const val USAGE_MEDIA = 1; const val USAGE_GAME = 14 }
}
class AudioPlaybackConfiguration(val clientUid: Int, val playerState: Int, val audioAttributes: AudioAttributes) {
    companion object { const val PLAYER_STATE_STARTED = 2 }
}
class AudioManager {
    open class AudioPlaybackCallback {
        open fun onPlaybackConfigChanged(configs: List<AudioPlaybackConfiguration>) {}
    }
    var callback: AudioPlaybackCallback? = null
    var activePlaybackConfigurations = emptyList<AudioPlaybackConfiguration>()
    var registers = 0
    var unregisters = 0
    fun registerAudioPlaybackCallback(cb: AudioPlaybackCallback, handler: Handler) { callback = cb; registers++ }
    fun unregisterAudioPlaybackCallback(cb: AudioPlaybackCallback) { callback = null; unregisters++ }
    fun playback(configs: List<AudioPlaybackConfiguration>) {
        activePlaybackConfigurations = configs
        callback?.onPlaybackConfigChanged(configs)
    }
}
''',
    'android/content/Context.kt': '''package android.content
import android.content.pm.PackageManager
import android.media.AudioManager
class ComponentName(val packageName: String)
class Context(val audio: AudioManager, val packageManager: PackageManager) {
    fun <T> getSystemService(type: Class<T>): T = type.cast(audio)
}
''',
    'android/content/pm/PackageManager.kt': '''package android.content.pm
class ApplicationInfo(val uid: Int, val category: Int) {
    companion object { const val CATEGORY_GAME = 0 }
}
class PackageManager {
    class NameNotFoundException : Exception()
    val apps = mutableMapOf<String, ApplicationInfo>()
    fun getApplicationInfo(name: String, flags: Int): ApplicationInfo = apps[name] ?: throw NameNotFoundException()
}
''',
    'android/app/ActivityTaskManager.kt': '''package android.app
import android.content.ComponentName
open class TaskStackListener { open fun onTaskStackChanged() {} }
class TaskInfo(val userId: Int, val topActivity: ComponentName?)
object ActivityTaskManager {
    var listener: TaskStackListener? = null
    var fail = false
    var task: TaskInfo? = null
    fun getInstance() = this
    fun getService() = this
    fun getTasks(count: Int): List<TaskInfo> {
        if (fail) throw IllegalStateException("Transient task query failure")
        return listOfNotNull(task)
    }
    fun registerTaskStackListener(value: TaskStackListener) { listener = value }
    fun unregisterTaskStackListener(value: TaskStackListener) { listener = null }
}
''',
}
