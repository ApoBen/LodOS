package org.lodos.app.session

import org.junit.Assert.*
import org.junit.Test

class SessionUiModelTest {
    @Test fun stoppedCanStart() {
        val ui = SessionUiModel.from(SessionState.Stopped)
        assertEquals("Stopped", ui.status); assertTrue(ui.startEnabled); assertFalse(ui.stopEnabled)
    }

    @Test fun startingDisablesStart() {
        val ui = SessionUiModel.from(SessionState.Starting)
        assertEquals("Starting…", ui.status); assertFalse(ui.startEnabled); assertTrue(ui.stopEnabled)
    }

    @Test fun runningCanStop() {
        val ui = SessionUiModel.from(SessionState.Running)
        assertEquals("Running", ui.status); assertFalse(ui.startEnabled); assertTrue(ui.stopEnabled)
    }

    @Test fun stoppingDisablesBothActions() {
        val ui = SessionUiModel.from(SessionState.Stopping)
        assertEquals("Stopping…", ui.status); assertFalse(ui.startEnabled); assertFalse(ui.stopEnabled)
    }

    @Test fun crashedShowsExitAndRetryDelay() {
        val ui = SessionUiModel.from(SessionState.Crashed(7, 2000))
        assertTrue(ui.status.contains("exit 7")); assertTrue(ui.status.contains("2000ms"))
    }

    @Test fun failedCanBeStartedAgain() {
        val ui = SessionUiModel.from(SessionState.Failed("spawn error"))
        assertTrue(ui.status.contains("spawn error")); assertTrue(ui.startEnabled); assertFalse(ui.stopEnabled)
    }
}
