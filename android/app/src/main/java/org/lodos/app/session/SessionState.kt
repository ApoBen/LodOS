package org.lodos.app.session

/** Guest session lifecycle (T048). */
sealed class SessionState {
    object Stopped : SessionState() { override fun toString() = "Stopped" }
    object Starting : SessionState() { override fun toString() = "Starting" }
    object Running : SessionState() { override fun toString() = "Running" }
    object Stopping : SessionState() { override fun toString() = "Stopping" }
    /** Unrequested non-zero exit; a backoff restart may be pending. */
    data class Crashed(val code: Int, val restartInMs: Long?) : SessionState()
    /** Gave up (restart budget exhausted or spawn error). */
    data class Failed(val reason: String) : SessionState()

    val isActive: Boolean get() = this is Starting || this is Running || this is Stopping
}
