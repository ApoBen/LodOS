package org.lodos.app.setup

/**
 * Pure-Kotlin coordinator that drives [RootfsRepo] (download -> sha256 verify -> extract)
 * and feeds progress/completion/error events into [SetupStateMachine].
 * Intended to be invoked off the UI thread.
 */
class RootfsInstaller(
    private val machine: SetupStateMachine,
    private val repo: RootfsRepo,
    private val spec: RootfsSpec
) {
    fun install(): SetupState {
        val initial = machine.state
        if (initial.step != SetupStep.ROOTFS || initial.rootfsStatus == RootfsStatus.READY || initial.isBusy) {
            return initial
        }

        val startEvent = if (initial.rootfsStatus == RootfsStatus.ERROR) {
            SetupEvent.RetryRootfs
        } else {
            SetupEvent.StartRootfsInstall
        }
        val started = machine.dispatch(startEvent)
        if (started.rootfsStatus != RootfsStatus.DOWNLOADING) {
            return started
        }

        val downloadResult = repo.download(spec.url) { pct ->
            machine.dispatch(SetupEvent.DownloadProgress(pct))
        }
        val archivePath = downloadResult.getOrElse { err ->
            return machine.dispatch(SetupEvent.RootfsFailed(err.message ?: "Download failed"))
        }

        val verifyResult = repo.verifySha256(archivePath, spec.sha256)
        verifyResult.onFailure { err ->
            return machine.dispatch(SetupEvent.RootfsFailed(err.message ?: "SHA-256 verification failed"))
        }

        machine.dispatch(SetupEvent.DownloadComplete(archivePath))

        val extractResult = repo.extract(archivePath) { pct ->
            machine.dispatch(SetupEvent.ExtractProgress(pct))
        }
        extractResult.onFailure { err ->
            return machine.dispatch(SetupEvent.RootfsFailed(err.message ?: "Extraction failed"))
        }

        return machine.dispatch(SetupEvent.ExtractComplete)
    }

    fun cancel() {
        repo.cancel()
    }
}
