package org.lodos.app.session

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestPaths
import org.lodos.app.guest.GuestSpec
import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.Tier

class SessionControllerTest {
    private val root = Files.createTempDirectory("sess").toFile()
    private val rootfs = File(root, "rootfs").apply { mkdirs() }
    private val logDir = File(root, "logs")

    class FakeProc(val exitOnTerm: Boolean) : GuestProcess {
        var terms = 0; var kills = 0; var exited = false
        override fun terminate() { terms++; if (exitOnTerm) exited = true }
        override fun kill() { kills++; exited = true }
        override fun waitFor(timeoutMs: Long) = exited
    }

    inner class FakeRunner(var exitOnTerm: Boolean = true, var fail: Boolean = false) : ProcessRunner {
        val procs = mutableListOf<FakeProc>()
        val exits = mutableListOf<(Int) -> Unit>()
        override fun start(argv: List<String>, env: Map<String, String>, log: File, onExit: (Int) -> Unit): GuestProcess {
            if (fail) throw java.io.IOException("no exec")
            exits += onExit
            return FakeProc(exitOnTerm).also { procs += it }
        }
        fun crash(code: Int = 1) = exits.last()(code)
    }

    class FakeClock : Clock {
        var now = 1_800_000_000_000L
        val pending = mutableListOf<Pair<Long, () -> Unit>>()
        override fun nowMs() = now
        override fun schedule(delayMs: Long, task: () -> Unit) { pending += delayMs to task }
        fun fire() { val p = pending.toList(); pending.clear(); p.forEach { now += it.first; it.second() } }
    }

    private val runner = FakeRunner()
    private val clock = FakeClock()
    private fun ctl(r: ProcessRunner = runner) = SessionController(r, clock, LogRotator(logDir))
    private fun spec(lite: Boolean = false) = GuestSpec(
        GuestPaths("/nlib", root.absolutePath, rootfs.absolutePath, "/files/run", "/cache/tmp"),
        lite = if (lite) LiteMode.profileFor(Tier.LOW) else null, lodosInit = true,
    )

    @Test fun startGoesRunning() {
        val c = ctl()
        val seen = mutableListOf<SessionState>()
        c.addListener { seen += it }
        assertTrue(c.start(spec()))
        assertEquals(SessionState.Running, c.state)
        assertEquals(listOf(SessionState.Starting, SessionState.Running), seen)
        assertEquals(1, runner.procs.size)
    }

    @Test fun stopGraceful() {
        val c = ctl(); c.start(spec())
        c.stop()
        assertEquals(SessionState.Stopped, c.state)
        assertEquals(1, runner.procs[0].terms)
        assertEquals(0, runner.procs[0].kills)
    }

    @Test fun stopTimeoutKills() {
        runner.exitOnTerm = false
        val c = ctl(); c.start(spec())
        c.stop()
        assertEquals(SessionState.Stopped, c.state)
        assertEquals(1, runner.procs[0].terms)
        assertEquals(1, runner.procs[0].kills)
    }

    @Test fun exitAfterStopIgnored() {
        val c = ctl(); c.start(spec())
        c.stop()
        runner.crash(143)
        assertEquals(SessionState.Stopped, c.state)
        assertTrue(clock.pending.isEmpty())
    }

    @Test fun crashRestartsWithBackoff() {
        val c = ctl(); c.start(spec())
        runner.crash(1)
        assertEquals(SessionState.Crashed(1, 1000), c.state)
        clock.fire()
        assertEquals(SessionState.Running, c.state)
        assertEquals(2, runner.procs.size)
        runner.crash(2)
        assertEquals(SessionState.Crashed(2, 2000), c.state)
        clock.fire()
        runner.crash(3)
        assertEquals(SessionState.Crashed(3, 4000), c.state)
    }

    @Test fun maxRestartsFails() {
        val c = ctl(); c.start(spec())
        repeat(3) { runner.crash(1); clock.fire() }
        assertEquals(4, runner.procs.size)
        runner.crash(1)
        assertTrue(c.state is SessionState.Failed)
        assertTrue(clock.pending.isEmpty())
        assertEquals(3, c.restarts)
    }

    @Test fun stableRunResetsBudget() {
        val c = ctl(); c.start(spec())
        repeat(3) { runner.crash(1); clock.fire() }
        clock.now += 120_000
        runner.crash(1)
        assertEquals(SessionState.Crashed(1, 1000), c.state)
    }

    @Test fun stopCancelsPendingRestart() {
        val c = ctl(); c.start(spec())
        runner.crash(1)
        c.stop()
        assertEquals(SessionState.Stopped, c.state)
        clock.fire()
        assertEquals(SessionState.Stopped, c.state)
        assertEquals(1, runner.procs.size)
    }

    @Test fun cleanExitIsStopped() {
        val c = ctl(); c.start(spec())
        runner.crash(0)
        assertEquals(SessionState.Stopped, c.state)
        assertTrue(clock.pending.isEmpty())
    }

    @Test fun doubleStartIgnored() {
        val c = ctl()
        assertTrue(c.start(spec()))
        assertFalse(c.start(spec()))
        assertEquals(1, runner.procs.size)
    }

    @Test fun argvEndsWithLodosInit() {
        val c = ctl(); c.start(spec())
        assertEquals(GuestLaunch.LODOS_INIT, c.lastArgv.last())
        assertEquals("/sbin/lodos-init", c.lastArgv.last())
        assertEquals("/nlib/libproot.so", c.lastArgv.first())
    }

    @Test fun liteEnvApplied() {
        val c = ctl(); c.start(spec(lite = true))
        assertEquals("1", c.lastEnv["LODOS_LITE"])
        assertEquals("0", c.lastEnv["GTK_ENABLE_ANIMATIONS"])
        assertTrue("LODOS_LITE=1" in c.lastArgv)
        assertEquals("/nlib/libproot_loader.so", c.lastEnv["PROOT_LOADER"])
    }

    @Test fun spawnErrorFails() {
        val c = ctl(FakeRunner(fail = true))
        assertFalse(c.start(spec()))
        assertTrue(c.state is SessionState.Failed)
        assertTrue(c.start(spec().copy()).not()) // still failing, but start allowed (not active)
    }

    @Test fun eachStartGetsNewLog() {
        val c = ctl(); c.start(spec())
        val first = c.currentLog!!
        assertTrue(first.exists() && first.name.startsWith("guest-") && first.parentFile == logDir)
        runner.crash(1); clock.fire()
        assertTrue(c.currentLog != first)
    }

    @Test fun logRotationKeepsFive() {
        val r = LogRotator(logDir)
        val made = (0 until 8).map { r.newLog(1_800_000_000_000L + it * 1000L) }
        val left = r.logs()
        assertEquals(5, left.size)
        assertEquals(made.takeLast(5), left)
        assertEquals(made.last(), r.newest())
    }

    @Test fun logRotationSameMsNoClobber() {
        val r = LogRotator(logDir)
        val a = r.newLog(5L); val b = r.newLog(5L)
        assertTrue(a != b && a.exists() && b.exists())
    }
}
