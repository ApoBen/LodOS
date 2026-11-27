package org.lodos.app.session

import java.io.File

/** Starts the packaged lodos-audio executable before the guest and owns it until guest shutdown. */
class AudioBridge(
    private val runner: ProcessRunner,
    private val logsDir: File,
    private val runDir: File,
    private val nativeLibDir: String,
) {
    companion object {
        const val SOCKET_NAME = "audio.sock"
        const val EXECUTABLE = "liblodos_audio.so"

        fun argv(nativeLibDir: String): List<String> = listOf("$nativeLibDir/$EXECUTABLE")
        fun socketPath(runDir: File): String = File(runDir, SOCKET_NAME).absolutePath
        fun env(socketPath: String): Map<String, String> = linkedMapOf(
            "LODOS_AUDIO_SOCK" to socketPath,
            "LODOS_AUDIO_RATE" to "48000",
            "LODOS_AUDIO_CH" to "2",
            "LODOS_AUDIO_BACKEND" to "aaudio",
        )
    }

    private var process: GuestProcess? = null

    @Synchronized fun start() {
        if (process != null) return
        logsDir.mkdirs()
        runDir.mkdirs()
        val socket = File(socketPath(runDir))
        socket.delete()
        process = runner.start(argv(nativeLibDir), env(socket.absolutePath), File(logsDir, "audio.log")) { }
        // Don't launch proot until the host has bound the path it will receive as a bind source.
        repeat(50) {
            if (socket.exists()) return
            Thread.sleep(20)
        }
    }

    @Synchronized fun stop() {
        val p = process ?: return
        process = null
        p.terminate()
        if (!p.waitFor(2_000)) {
            p.kill()
            p.waitFor(2_000)
        }
        File(runDir, SOCKET_NAME).delete()
    }
}
