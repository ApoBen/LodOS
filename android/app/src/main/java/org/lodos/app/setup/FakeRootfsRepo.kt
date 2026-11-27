package org.lodos.app.setup

import java.util.concurrent.atomic.AtomicBoolean

/**
 * Deterministic in-memory [RootfsRepo] fake for UI scaffolding and JVM unit tests.
 */
class FakeRootfsRepo(
    initiallyInstalled: Boolean = false,
    private val stepDelayMs: Long = 0L,
    private val progressSteps: List<Int> = listOf(0, 25, 50, 75, 100),
    var archivePathToReturn: String = "/fake/cache/debian-trixie-arm64.tar.gz",
    var downloadFailure: Throwable? = null,
    var verifyFailure: Throwable? = null,
    var extractFailure: Throwable? = null
) : RootfsRepo {

    @Volatile
    private var installed: Boolean = initiallyInstalled
    private val cancelled = AtomicBoolean(false)

    val downloadedUrls = mutableListOf<String>()
    val verifiedPairs = mutableListOf<Pair<String, String>>()
    val extractedPaths = mutableListOf<String>()

    override fun isInstalled(): Boolean = installed

    override fun download(url: String, onProgress: (Int) -> Unit): Result<String> {
        cancelled.set(false)
        downloadedUrls.add(url)
        for (pct in progressSteps) {
            if (cancelled.get()) {
                return Result.failure(IllegalStateException("Download cancelled"))
            }
            onProgress(pct)
            downloadFailure?.let { if (pct >= 50 || progressSteps.size == 1) return Result.failure(it) }
            sleepIfConfigured()
        }
        downloadFailure?.let { return Result.failure(it) }
        return Result.success(archivePathToReturn)
    }

    override fun verifySha256(archivePath: String, expectedSha256: String): Result<Unit> {
        if (cancelled.get()) {
            return Result.failure(IllegalStateException("Verification cancelled"))
        }
        verifiedPairs.add(archivePath to expectedSha256)
        verifyFailure?.let { return Result.failure(it) }
        if (expectedSha256.isBlank()) {
            return Result.failure(IllegalArgumentException("Expected SHA-256 is blank"))
        }
        return Result.success(Unit)
    }

    override fun extract(archivePath: String, onProgress: (Int) -> Unit): Result<Unit> {
        extractedPaths.add(archivePath)
        for (pct in progressSteps) {
            if (cancelled.get()) {
                return Result.failure(IllegalStateException("Extraction cancelled"))
            }
            onProgress(pct)
            extractFailure?.let { if (pct >= 50 || progressSteps.size == 1) return Result.failure(it) }
            sleepIfConfigured()
        }
        extractFailure?.let { return Result.failure(it) }
        installed = true
        return Result.success(Unit)
    }

    override fun cancel() {
        cancelled.set(true)
    }

    private fun sleepIfConfigured() {
        if (stepDelayMs > 0L) {
            try {
                Thread.sleep(stepDelayMs)
            } catch (_: InterruptedException) {
                cancelled.set(true)
                Thread.currentThread().interrupt()
            }
        }
    }
}
