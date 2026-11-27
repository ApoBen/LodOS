package org.lodos.app.session

import java.io.File
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestSpec

/** Handle to a spawned guest process (proot). */
interface GuestProcess {
    /** SIGTERM. */
    fun terminate()
    /** SIGKILL. */
    fun kill()
    /** true if exited within [timeoutMs]. */
    fun waitFor(timeoutMs: Long): Boolean
}

/** Spawns argv with a clean env; stdout+stderr appended to [log]; [onExit] called once with the exit code. */
fun interface ProcessRunner {
    fun start(argv: List<String>, env: Map<String, String>, log: File, onExit: (Int) -> Unit): GuestProcess
}

interface Clock {
    fun nowMs(): Long
    /** Run [task] after [delayMs] (restart backoff). */
    fun schedule(delayMs: Long, task: () -> Unit)
}

/**
 * Owns the guest lifetime (T048). Pure Kotlin, no Android deps.
 * start: proot ... /sbin/lodos-init (spec.lodosInit) with lite env; stop: SIGTERM, then SIGKILL after
 * [stopTimeoutMs]; unrequested non-zero exit -> backoff restart (base*2^n), max [maxRestarts] -> Failed.
 * Exit code 0 without stop request = guest powered off -> Stopped. A run longer than [stableMs] resets the budget.
 */
class SessionController(
    private val runner: ProcessRunner,
    private val clock: Clock,
    private val logs: LogRotator,
    val maxRestarts: Int = 3,
    val backoffBaseMs: Long = 1000,
    val stopTimeoutMs: Long = 5000,
    val stableMs: Long = 60_000,
) {
    var state: SessionState = SessionState.Stopped
        private set
    var restarts = 0
        private set
    var currentLog: File? = null
        private set
    var lastArgv: List<String> = emptyList()
        private set
    var lastEnv: Map<String, String> = emptyMap()
        private set

    private var spec: GuestSpec? = null
    private var proc: GuestProcess? = null
    private var generation = 0
    private var startedAtMs = 0L
    private val listeners = mutableListOf<(SessionState) -> Unit>()

    @Synchronized fun addListener(l: (SessionState) -> Unit) { listeners += l }
    @Synchronized fun removeListener(l: (SessionState) -> Unit) { listeners -= l }

    private fun set(s: SessionState) {
        state = s
        listeners.toList().forEach { it(s) }
    }

    /** false if already active (double start ignored). */
    @Synchronized fun start(spec: GuestSpec): Boolean {
        if (state.isActive) return false
        this.spec = spec
        restarts = 0
        return launch()
    }

    private fun launch(): Boolean {
        val s = spec ?: return false
        set(SessionState.Starting)
        val gen = ++generation
        return try {
            val argv = GuestLaunch.buildArgv(s)
            val env = GuestLaunch.buildEnv(s)
            val log = logs.newLog(clock.nowMs())
            lastArgv = argv; lastEnv = env; currentLog = log
            startedAtMs = clock.nowMs()
            proc = runner.start(argv, env, log) { code -> onExit(gen, code) }
            if (state == SessionState.Starting && generation == gen) set(SessionState.Running)
            true
        } catch (e: Exception) {
            proc = null
            set(SessionState.Failed("spawn: ${e.message}"))
            false
        }
    }

    @Synchronized private fun onExit(gen: Int, code: Int) {
        if (gen != generation) return // stale or stop-requested
        proc = null
        if (state == SessionState.Stopping) return
        if (code == 0) { set(SessionState.Stopped); return }
        if (clock.nowMs() - startedAtMs >= stableMs) restarts = 0
        if (restarts >= maxRestarts) {
            set(SessionState.Failed("exit $code after $restarts restarts"))
            return
        }
        val delay = backoffBaseMs shl restarts
        restarts++
        set(SessionState.Crashed(code, delay))
        val g = generation
        clock.schedule(delay) { restartIfPending(g) }
    }

    @Synchronized private fun restartIfPending(gen: Int) {
        if (gen == generation && state is SessionState.Crashed) launch()
    }

    /** Blocking: SIGTERM, wait [stopTimeoutMs], SIGKILL. Cancels pending restarts. */
    @Synchronized fun stop() {
        val p = proc
        generation++ // invalidate exit callbacks + pending restart
        if (p == null) {
            if (state !is SessionState.Failed) set(SessionState.Stopped)
            return
        }
        set(SessionState.Stopping)
        p.terminate()
        if (!p.waitFor(stopTimeoutMs)) {
            p.kill()
            p.waitFor(stopTimeoutMs)
        }
        proc = null
        set(SessionState.Stopped)
    }
}
