package org.lodos.app.setup

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RootfsInstallerTest {

    private val testSpec = RootfsSpec(
        url = "https://example.invalid/debian-trixie-arm64.tar.gz",
        sha256 = "deadbeefcafebabe0123456789abcdef"
    )

    @Test
    fun installHappyPathRunsDownloadVerifyExtractAndPersistsReady() {
        val store = InMemorySetupStore()
        val repo = FakeRootfsRepo(progressSteps = listOf(0, 50, 100))
        val machine = SetupStateMachine(store, rootfsInstalled = { repo.isInstalled() })
        val installer = RootfsInstaller(machine, repo, testSpec)

        val states = mutableListOf<SetupState>()
        machine.addListener { states.add(it) }

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        val result = installer.install()

        assertEquals(SetupStep.ROOTFS, result.step)
        assertEquals(RootfsStatus.READY, result.rootfsStatus)
        assertEquals(100, result.progress)
        assertTrue(repo.isInstalled())
        assertEquals(listOf(testSpec.url), repo.downloadedUrls)
        assertEquals(listOf(repo.archivePathToReturn to testSpec.sha256), repo.verifiedPairs)
        assertEquals(listOf(repo.archivePathToReturn), repo.extractedPaths)

        assertTrue(states.any { it.rootfsStatus == RootfsStatus.DOWNLOADING && it.progress == 50 })
        assertTrue(states.any { it.rootfsStatus == RootfsStatus.EXTRACTING && it.progress == 50 })

        // Advance to Start and simulate process death -> resumes at START/READY
        machine.dispatch(SetupEvent.ProceedToStart)
        val resumed = SetupStateMachine(store, rootfsInstalled = { repo.isInstalled() })
        assertEquals(SetupStep.START, resumed.state.step)
        assertEquals(RootfsStatus.READY, resumed.state.rootfsStatus)
        assertEquals(100, resumed.state.progress)
    }

    @Test
    fun downloadFailureStopsBeforeVerifyAndExtract() {
        val store = InMemorySetupStore()
        val repo = FakeRootfsRepo(downloadFailure = RuntimeException("HTTP 503"))
        val machine = SetupStateMachine(store, rootfsInstalled = { repo.isInstalled() })
        val installer = RootfsInstaller(machine, repo, testSpec)

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        val result = installer.install()

        assertEquals(RootfsStatus.ERROR, result.rootfsStatus)
        assertEquals("HTTP 503", result.errorMessage)
        assertFalse(repo.isInstalled())
        assertTrue(repo.verifiedPairs.isEmpty())
        assertTrue(repo.extractedPaths.isEmpty())
    }

    @Test
    fun sha256FailureStopsBeforeExtract() {
        val store = InMemorySetupStore()
        val repo = FakeRootfsRepo(verifyFailure = IllegalStateException("SHA-256 mismatch"))
        val machine = SetupStateMachine(store, rootfsInstalled = { repo.isInstalled() })
        val installer = RootfsInstaller(machine, repo, testSpec)

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        val result = installer.install()

        assertEquals(RootfsStatus.ERROR, result.rootfsStatus)
        assertEquals("SHA-256 mismatch", result.errorMessage)
        assertFalse(repo.isInstalled())
        assertEquals(1, repo.verifiedPairs.size)
        assertTrue(repo.extractedPaths.isEmpty())
    }

    @Test
    fun extractFailureCanBeRetriedSuccessfully() {
        val store = InMemorySetupStore()
        val repo = FakeRootfsRepo(extractFailure = RuntimeException("No space left"))
        val machine = SetupStateMachine(store, rootfsInstalled = { repo.isInstalled() })
        val installer = RootfsInstaller(machine, repo, testSpec)

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        val failed = installer.install()
        assertEquals(RootfsStatus.ERROR, failed.rootfsStatus)
        assertEquals("No space left", failed.errorMessage)
        assertFalse(repo.isInstalled())

        // Clear fault and retry
        repo.extractFailure = null
        val retried = installer.install()
        assertEquals(RootfsStatus.READY, retried.rootfsStatus)
        assertEquals(100, retried.progress)
        assertTrue(repo.isInstalled())
    }
}
