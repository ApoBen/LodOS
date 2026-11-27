package org.lodos.app.session

/** Text and control state presented for a guest lifecycle state. */
data class SessionUiModel(
    val status: String,
    val startEnabled: Boolean,
    val stopEnabled: Boolean,
    val startLabel: String = "Start session",
    val stopLabel: String = "Stop session",
) {
    companion object {
        fun from(state: SessionState): SessionUiModel = when (state) {
            SessionState.Stopped -> SessionUiModel("Stopped", true, false)
            SessionState.Starting -> SessionUiModel("Starting…", false, true)
            SessionState.Running -> SessionUiModel("Running", false, true)
            SessionState.Stopping -> SessionUiModel("Stopping…", false, false)
            is SessionState.Crashed -> SessionUiModel(
                "Crashed (exit ${state.code}); restarting${state.restartInMs?.let { " in ${it}ms" } ?: ""}", false, true
            )
            is SessionState.Failed -> SessionUiModel("Failed: ${state.reason}", true, false)
        }
    }
}
