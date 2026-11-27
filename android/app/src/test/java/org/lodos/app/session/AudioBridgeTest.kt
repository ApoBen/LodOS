package org.lodos.app.session

import java.io.File
import org.junit.Assert.*
import org.junit.Test
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestPaths
import org.lodos.app.guest.GuestSpec

class AudioBridgeTest {
    private class FakeProcess(val events: MutableList<String>) : GuestProcess {
        override fun terminate() { events += "terminate" }
        override fun kill() { events += "kill" }
        override fun waitFor(timeoutMs: Long): Boolean { events += "wait"; return true }
    }

    private class FakeRunner(val events: MutableList<String>) : ProcessRunner {
        var argv = emptyList<String>()
        var env = emptyMap<String, String>()
        override fun start(argv: List<String>, env: Map<String, String>, log: File, onExit: (Int) -> Unit): GuestProcess {
            events += "start:${argv.single()}"
            this.argv = argv
            this.env = env
            File(env.getValue("LODOS_AUDIO_SOCK")).apply { parentFile?.mkdirs(); createNewFile() }
            return FakeProcess(events)
        }
    }

    private fun bridge(events: MutableList<String>, dir: File): Pair<AudioBridge, FakeRunner> {
        val runner = FakeRunner(events)
        return AudioBridge(runner, File(dir, "logs"), File(dir, "run"), "/native") to runner
    }

    @Test fun argvNamesPackagedExecutable() {
        assertEquals(listOf("/native/liblodos_audio.so"), AudioBridge.argv("/native"))
    }

    @Test fun socketPathLivesUnderFilesRunDirectory() {
        val dir = createTempDir()
        try {
            val (audio, runner) = bridge(mutableListOf(), dir)
            audio.start()
            assertEquals(AudioBridge.socketPath(File(dir, "run")), runner.env["LODOS_AUDIO_SOCK"])
            audio.stop()
        } finally { dir.deleteRecursively() }
    }

    @Test fun guestGetsAudioBindAndPulseServer() {
        val root = createTempDir()
        try {
            val paths = GuestPaths("/native", root.absolutePath, File(root, "root").apply { mkdirs() }.absolutePath,
                File(root, "run").apply { mkdirs() }.absolutePath, File(root, "tmp").apply { mkdirs() }.absolutePath)
            val argv = GuestLaunch.buildArgv(GuestSpec(paths, listOf("true"),
                extraBinds = listOf("${paths.runDir}/audio.sock" to GuestLaunch.GUEST_AUDIO_SOCKET)))
            assertTrue(argv.contains("${paths.runDir}/audio.sock:${GuestLaunch.GUEST_AUDIO_SOCKET}"))
            assertEquals("unix:${GuestLaunch.GUEST_PULSE_SOCKET}", GuestLaunch.guestEnv(GuestSpec(paths, listOf("true")))["PULSE_SERVER"])
        } finally { root.deleteRecursively() }
    }

    @Test fun daemonStopsWithTerminateAndWait() {
        val dir = createTempDir()
        val events = mutableListOf<String>()
        try {
            val (audio, _) = bridge(events, dir)
            audio.start()
            audio.stop()
            assertEquals(listOf("start:/native/liblodos_audio.so", "terminate", "wait"), events)
        } finally { dir.deleteRecursively() }
    }
}
