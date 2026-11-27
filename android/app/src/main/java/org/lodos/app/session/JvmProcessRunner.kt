package org.lodos.app.session

import java.io.File
import java.util.concurrent.TimeUnit

/** java.lang.Process backed runner: destroy() = SIGTERM, destroyForcibly() = SIGKILL. */
class JvmProcessRunner : ProcessRunner {
    override fun start(argv: List<String>, env: Map<String, String>, log: File, onExit: (Int) -> Unit): GuestProcess {
        val pb = ProcessBuilder(argv).redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(log))
        pb.environment().apply { clear(); putAll(env) }
        val p = pb.start()
        Thread({ onExit(runCatching { p.waitFor() }.getOrDefault(-1)) }, "guest-wait").apply { isDaemon = true }.start()
        return object : GuestProcess {
            override fun terminate() = p.destroy()
            override fun kill() { p.destroyForcibly() }
            override fun waitFor(timeoutMs: Long) = p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)
        }
    }
}

/** Real clock; schedule on a daemon timer thread. */
class SystemClock : Clock {
    private val timer = java.util.Timer("guest-backoff", true)
    override fun nowMs() = System.currentTimeMillis()
    override fun schedule(delayMs: Long, task: () -> Unit) {
        timer.schedule(object : java.util.TimerTask() { override fun run() = task() }, delayMs)
    }
}
