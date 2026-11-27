package org.lodos.app.setup

enum class SetupStep {
    WELCOME,
    ROOTFS,
    START
}

enum class RootfsStatus {
    MISSING,
    DOWNLOADING,
    EXTRACTING,
    READY,
    ERROR
}

sealed interface SetupEvent {
    data object ContinueFromWelcome : SetupEvent
    data object Back : SetupEvent
    data object StartRootfsInstall : SetupEvent
    data class DownloadProgress(val percent: Int) : SetupEvent
    data class DownloadComplete(val archivePath: String) : SetupEvent
    data class ExtractProgress(val percent: Int) : SetupEvent
    data object ExtractComplete : SetupEvent
    data class RootfsFailed(val message: String) : SetupEvent
    data object RetryRootfs : SetupEvent
    data object ProceedToStart : SetupEvent
    data object Reset : SetupEvent
}

/**
 * Pure-Kotlin immutable state for the P1 setup wizard.
 */
data class SetupState(
    val step: SetupStep = SetupStep.WELCOME,
    val rootfsStatus: RootfsStatus = RootfsStatus.MISSING,
    val progress: Int = 0,
    val errorMessage: String? = null,
    val archivePath: String? = null
) {
    val isBusy: Boolean
        get() = rootfsStatus == RootfsStatus.DOWNLOADING || rootfsStatus == RootfsStatus.EXTRACTING

    companion object {
        const val INTERRUPTED_MESSAGE = "Previous installation was interrupted"

        /**
         * Pure state-machine transition function. Invalid transitions return [state] unchanged.
         */
        fun reduce(state: SetupState, event: SetupEvent): SetupState = when (event) {
            SetupEvent.ContinueFromWelcome -> {
                if (state.step == SetupStep.WELCOME) {
                    state.copy(step = SetupStep.ROOTFS)
                } else {
                    state
                }
            }

            SetupEvent.Back -> when (state.step) {
                SetupStep.WELCOME -> state
                SetupStep.ROOTFS -> if (state.isBusy) state else state.copy(step = SetupStep.WELCOME)
                SetupStep.START -> state.copy(step = SetupStep.ROOTFS)
            }

            SetupEvent.StartRootfsInstall -> {
                if (state.step == SetupStep.ROOTFS &&
                    (state.rootfsStatus == RootfsStatus.MISSING || state.rootfsStatus == RootfsStatus.ERROR)
                ) {
                    state.copy(
                        rootfsStatus = RootfsStatus.DOWNLOADING,
                        progress = 0,
                        errorMessage = null,
                        archivePath = null
                    )
                } else {
                    state
                }
            }

            is SetupEvent.DownloadProgress -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.DOWNLOADING) {
                    state.copy(progress = event.percent.coerceIn(0, 100))
                } else {
                    state
                }
            }

            is SetupEvent.DownloadComplete -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.DOWNLOADING) {
                    state.copy(
                        rootfsStatus = RootfsStatus.EXTRACTING,
                        progress = 0,
                        archivePath = event.archivePath,
                        errorMessage = null
                    )
                } else {
                    state
                }
            }

            is SetupEvent.ExtractProgress -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.EXTRACTING) {
                    state.copy(progress = event.percent.coerceIn(0, 100))
                } else {
                    state
                }
            }

            SetupEvent.ExtractComplete -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.EXTRACTING) {
                    state.copy(
                        rootfsStatus = RootfsStatus.READY,
                        progress = 100,
                        errorMessage = null
                    )
                } else {
                    state
                }
            }

            is SetupEvent.RootfsFailed -> {
                if (state.step == SetupStep.ROOTFS && state.isBusy) {
                    state.copy(
                        rootfsStatus = RootfsStatus.ERROR,
                        progress = 0,
                        errorMessage = event.message.ifBlank { "Rootfs setup failed" }
                    )
                } else {
                    state
                }
            }

            SetupEvent.RetryRootfs -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.ERROR) {
                    state.copy(
                        rootfsStatus = RootfsStatus.DOWNLOADING,
                        progress = 0,
                        errorMessage = null,
                        archivePath = null
                    )
                } else {
                    state
                }
            }

            SetupEvent.ProceedToStart -> {
                if (state.step == SetupStep.ROOTFS && state.rootfsStatus == RootfsStatus.READY) {
                    state.copy(step = SetupStep.START)
                } else {
                    state
                }
            }

            SetupEvent.Reset -> SetupState()
        }

        /**
         * Reconciles persisted state with actual on-disk rootfs presence after process death.
         */
        fun reconcileOnResume(persisted: SetupState, rootfsInstalled: Boolean): SetupState {
            if (rootfsInstalled) {
                return persisted.copy(
                    rootfsStatus = RootfsStatus.READY,
                    progress = 100,
                    errorMessage = null
                )
            }
            if (persisted.step == SetupStep.START || persisted.rootfsStatus == RootfsStatus.READY) {
                return SetupState(
                    step = SetupStep.ROOTFS,
                    rootfsStatus = RootfsStatus.MISSING,
                    progress = 0,
                    errorMessage = null,
                    archivePath = null
                )
            }
            if (persisted.isBusy) {
                return SetupState(
                    step = SetupStep.ROOTFS,
                    rootfsStatus = RootfsStatus.MISSING,
                    progress = 0,
                    errorMessage = INTERRUPTED_MESSAGE,
                    archivePath = null
                )
            }
            return persisted.copy(progress = 0)
        }
    }
}

/**
 * Stateful, persisted runner around [SetupState.reduce] and [SetupStore].
 */
class SetupStateMachine(
    private val store: SetupStore,
    private val rootfsInstalled: () -> Boolean = { false }
) {
    private val lock = Any()
    private val listeners = mutableListOf<(SetupState) -> Unit>()

    @Volatile
    private var currentState: SetupState

    val state: SetupState
        get() = currentState

    init {
        val loaded = loadFromStore()
        val reconciled = SetupState.reconcileOnResume(loaded, rootfsInstalled())
        currentState = reconciled
        if (reconciled != loaded || !store.getBoolean(KEY_INITIALIZED, false)) {
            persistToStore(reconciled)
        }
    }

    fun dispatch(event: SetupEvent): SetupState {
        val updated: SetupState
        val snapshot: List<(SetupState) -> Unit>
        synchronized(lock) {
            val next = SetupState.reduce(currentState, event)
            if (next == currentState) {
                return currentState
            }
            currentState = next
            persistToStore(next)
            updated = next
            snapshot = listeners.toList()
        }
        for (listener in snapshot) {
            listener(updated)
        }
        return updated
    }

    fun addListener(listener: (SetupState) -> Unit) {
        val current: SetupState
        synchronized(lock) {
            listeners.add(listener)
            current = currentState
        }
        listener(current)
    }

    fun removeListener(listener: (SetupState) -> Unit) {
        synchronized(lock) {
            listeners.remove(listener)
        }
    }

    private fun loadFromStore(): SetupState {
        val step = store.getString(KEY_STEP)
            ?.let { runCatching { SetupStep.valueOf(it) }.getOrNull() }
            ?: SetupStep.WELCOME
        val status = store.getString(KEY_ROOTFS_STATUS)
            ?.let { runCatching { RootfsStatus.valueOf(it) }.getOrNull() }
            ?: RootfsStatus.MISSING
        val progress = store.getInt(KEY_PROGRESS, 0).coerceIn(0, 100)
        val error = store.getString(KEY_ERROR_MESSAGE)
        val archive = store.getString(KEY_ARCHIVE_PATH)
        return SetupState(
            step = step,
            rootfsStatus = status,
            progress = progress,
            errorMessage = error,
            archivePath = archive
        )
    }

    private fun persistToStore(s: SetupState) {
        store.putBoolean(KEY_INITIALIZED, true)
        store.putString(KEY_STEP, s.step.name)
        store.putString(KEY_ROOTFS_STATUS, s.rootfsStatus.name)
        store.putInt(KEY_PROGRESS, s.progress)
        store.putString(KEY_ERROR_MESSAGE, s.errorMessage)
        store.putString(KEY_ARCHIVE_PATH, s.archivePath)
    }

    companion object {
        const val KEY_INITIALIZED = "setup_initialized"
        const val KEY_STEP = "setup_step"
        const val KEY_ROOTFS_STATUS = "setup_rootfs_status"
        const val KEY_PROGRESS = "setup_progress"
        const val KEY_ERROR_MESSAGE = "setup_error_message"
        const val KEY_ARCHIVE_PATH = "setup_archive_path"
    }
}
