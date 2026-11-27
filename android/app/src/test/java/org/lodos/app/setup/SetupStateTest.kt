package org.lodos.app.setup

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class InMemorySetupStore : SetupStore {
    private val strings = mutableMapOf<String, String>()
    private val ints = mutableMapOf<String, Int>()
    private val booleans = mutableMapOf<String, Boolean>()

    override fun getString(key: String, default: String?): String? = strings[key] ?: default

    override fun putString(key: String, value: String?) {
        if (value == null) strings.remove(key) else strings[key] = value
    }

    override fun getInt(key: String, default: Int): Int = ints[key] ?: default

    override fun putInt(key: String, value: Int) {
        ints[key] = value
    }

    override fun getBoolean(key: String, default: Boolean): Boolean = booleans[key] ?: default

    override fun putBoolean(key: String, value: Boolean) {
        booleans[key] = value
    }

    override fun clear() {
        strings.clear()
        ints.clear()
        booleans.clear()
    }
}

class SetupStateTest {

    @Test
    fun happyPathTransitionsFromWelcomeToStart() {
        val store = InMemorySetupStore()
        val machine = SetupStateMachine(store)
        val observed = mutableListOf<SetupState>()
        machine.addListener { observed.add(it) }

        assertEquals(SetupState(SetupStep.WELCOME, RootfsStatus.MISSING, 0), machine.state)

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        assertEquals(SetupStep.ROOTFS, machine.state.step)
        assertEquals(RootfsStatus.MISSING, machine.state.rootfsStatus)

        machine.dispatch(SetupEvent.StartRootfsInstall)
        assertEquals(RootfsStatus.DOWNLOADING, machine.state.rootfsStatus)
        assertEquals(0, machine.state.progress)
        assertTrue(machine.state.isBusy)

        machine.dispatch(SetupEvent.DownloadProgress(42))
        assertEquals(42, machine.state.progress)

        machine.dispatch(SetupEvent.DownloadComplete("/cache/rootfs.tar.gz"))
        assertEquals(RootfsStatus.EXTRACTING, machine.state.rootfsStatus)
        assertEquals(0, machine.state.progress)
        assertEquals("/cache/rootfs.tar.gz", machine.state.archivePath)
        assertTrue(machine.state.isBusy)

        machine.dispatch(SetupEvent.ExtractProgress(75))
        assertEquals(75, machine.state.progress)

        machine.dispatch(SetupEvent.ExtractComplete)
        assertEquals(RootfsStatus.READY, machine.state.rootfsStatus)
        assertEquals(100, machine.state.progress)
        assertFalse(machine.state.isBusy)

        machine.dispatch(SetupEvent.ProceedToStart)
        assertEquals(SetupStep.START, machine.state.step)
        assertEquals(RootfsStatus.READY, machine.state.rootfsStatus)
        assertEquals(100, machine.state.progress)

        // Initial emission + 7 distinct transitions
        assertEquals(8, observed.size)
    }

    @Test
    fun progressIsClampedToZeroHundred() {
        var s = SetupState(SetupStep.ROOTFS, RootfsStatus.DOWNLOADING, 0)
        s = SetupState.reduce(s, SetupEvent.DownloadProgress(-15))
        assertEquals(0, s.progress)
        s = SetupState.reduce(s, SetupEvent.DownloadProgress(250))
        assertEquals(100, s.progress)

        s = SetupState.reduce(s, SetupEvent.DownloadComplete("/tmp/a.tar.gz"))
        s = SetupState.reduce(s, SetupEvent.ExtractProgress(-1))
        assertEquals(0, s.progress)
        s = SetupState.reduce(s, SetupEvent.ExtractProgress(120))
        assertEquals(100, s.progress)
    }

    @Test
    fun errorAndRetryTransitions() {
        val store = InMemorySetupStore()
        val machine = SetupStateMachine(store)

        machine.dispatch(SetupEvent.ContinueFromWelcome)
        machine.dispatch(SetupEvent.StartRootfsInstall)
        machine.dispatch(SetupEvent.DownloadProgress(30))
        machine.dispatch(SetupEvent.RootfsFailed("Network timeout"))

        assertEquals(RootfsStatus.ERROR, machine.state.rootfsStatus)
        assertEquals(0, machine.state.progress)
        assertEquals("Network timeout", machine.state.errorMessage)
        assertFalse(machine.state.isBusy)

        machine.dispatch(SetupEvent.RetryRootfs)
        assertEquals(RootfsStatus.DOWNLOADING, machine.state.rootfsStatus)
        assertNull(machine.state.errorMessage)
        assertEquals(0, machine.state.progress)

        machine.dispatch(SetupEvent.DownloadComplete("/cache/r.tar.gz"))
        machine.dispatch(SetupEvent.RootfsFailed("Corrupt archive"))
        assertEquals(RootfsStatus.ERROR, machine.state.rootfsStatus)
        assertEquals("Corrupt archive", machine.state.errorMessage)

        // StartRootfsInstall also restarts from ERROR
        machine.dispatch(SetupEvent.StartRootfsInstall)
        assertEquals(RootfsStatus.DOWNLOADING, machine.state.rootfsStatus)
        assertNull(machine.state.errorMessage)
    }

    @Test
    fun backNavigationAndInvalidTransitionsAreGuarded() {
        val store = InMemorySetupStore()
        val machine = SetupStateMachine(store)

        // Back on WELCOME is a no-op
        machine.dispatch(SetupEvent.Back)
        assertEquals(SetupStep.WELCOME, machine.state.step)

        // Cannot install or proceed from WELCOME
        machine.dispatch(SetupEvent.StartRootfsInstall)
        machine.dispatch(SetupEvent.ProceedToStart)
        assertEquals(SetupStep.WELCOME, machine.state.step)
        assertEquals(RootfsStatus.MISSING, machine.state.rootfsStatus)

        // Move to ROOTFS and back to WELCOME
        machine.dispatch(SetupEvent.ContinueFromWelcome)
        machine.dispatch(SetupEvent.Back)
        assertEquals(SetupStep.WELCOME, machine.state.step)

        // Move to ROOTFS, start download -> Back & ProceedToStart must be ignored
        machine.dispatch(SetupEvent.ContinueFromWelcome)
        machine.dispatch(SetupEvent.StartRootfsInstall)
        machine.dispatch(SetupEvent.Back)
        machine.dispatch(SetupEvent.ProceedToStart)
        machine.dispatch(SetupEvent.ExtractProgress(50)) // wrong phase
        assertEquals(SetupStep.ROOTFS, machine.state.step)
        assertEquals(RootfsStatus.DOWNLOADING, machine.state.rootfsStatus)
        assertEquals(0, machine.state.progress)

        // Complete -> ProceedToStart -> Back returns to ROOTFS (still READY)
        machine.dispatch(SetupEvent.DownloadComplete("/cache/r.tar.gz"))
        machine.dispatch(SetupEvent.Back) // ignored while extracting
        assertEquals(SetupStep.ROOTFS, machine.state.step)
        machine.dispatch(SetupEvent.ExtractComplete)
        machine.dispatch(SetupEvent.ProceedToStart)
        assertEquals(SetupStep.START, machine.state.step)

        machine.dispatch(SetupEvent.Back)
        assertEquals(SetupStep.ROOTFS, machine.state.step)
        assertEquals(RootfsStatus.READY, machine.state.rootfsStatus)

        // Reset clears everything
        machine.dispatch(SetupEvent.Reset)
        assertEquals(SetupState(), machine.state)
    }

    @Test
    fun resumeAfterProcessDeathAtEachStep() {
        val store = InMemorySetupStore()

        // 1. Die on ROOTFS MISSING -> resumes on ROOTFS MISSING
        val m1 = SetupStateMachine(store, rootfsInstalled = { false })
        m1.dispatch(SetupEvent.ContinueFromWelcome)
        val m2 = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.ROOTFS, m2.state.step)
        assertEquals(RootfsStatus.MISSING, m2.state.rootfsStatus)

        // 2. Die mid-DOWNLOADING -> recovers to ROOTFS MISSING with interrupted note
        m2.dispatch(SetupEvent.StartRootfsInstall)
        m2.dispatch(SetupEvent.DownloadProgress(65))
        val m3 = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.ROOTFS, m3.state.step)
        assertEquals(RootfsStatus.MISSING, m3.state.rootfsStatus)
        assertEquals(0, m3.state.progress)
        assertEquals(SetupState.INTERRUPTED_MESSAGE, m3.state.errorMessage)

        // 3. Die mid-EXTRACTING with rootfs NOT on disk -> recovers to ROOTFS MISSING
        m3.dispatch(SetupEvent.StartRootfsInstall)
        m3.dispatch(SetupEvent.DownloadComplete("/cache/rootfs.tar.gz"))
        m3.dispatch(SetupEvent.ExtractProgress(40))
        val m4 = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.ROOTFS, m4.state.step)
        assertEquals(RootfsStatus.MISSING, m4.state.rootfsStatus)
        assertEquals(SetupState.INTERRUPTED_MESSAGE, m4.state.errorMessage)

        // 4. Die on ERROR -> preserves error message on resume
        m4.dispatch(SetupEvent.StartRootfsInstall)
        m4.dispatch(SetupEvent.RootfsFailed("Checksum mismatch"))
        val m5 = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.ROOTFS, m5.state.step)
        assertEquals(RootfsStatus.ERROR, m5.state.rootfsStatus)
        assertEquals("Checksum mismatch", m5.state.errorMessage)

        // 5. Complete and advance to START -> die and resume with rootfs on disk -> stays on START
        m5.dispatch(SetupEvent.RetryRootfs)
        m5.dispatch(SetupEvent.DownloadComplete("/cache/rootfs.tar.gz"))
        m5.dispatch(SetupEvent.ExtractComplete)
        m5.dispatch(SetupEvent.ProceedToStart)
        val m6 = SetupStateMachine(store, rootfsInstalled = { true })
        assertEquals(SetupStep.START, m6.state.step)
        assertEquals(RootfsStatus.READY, m6.state.rootfsStatus)
        assertEquals(100, m6.state.progress)
        assertNull(m6.state.errorMessage)
    }

    @Test
    fun resumeReconcilesWithOnDiskRootfsPresence() {
        val store = InMemorySetupStore()

        // Persisted START/READY, but rootfs was wiped externally -> falls back to ROOTFS MISSING
        val m1 = SetupStateMachine(store, rootfsInstalled = { false })
        m1.dispatch(SetupEvent.ContinueFromWelcome)
        m1.dispatch(SetupEvent.StartRootfsInstall)
        m1.dispatch(SetupEvent.DownloadComplete("/cache/r.tar.gz"))
        m1.dispatch(SetupEvent.ExtractComplete)
        m1.dispatch(SetupEvent.ProceedToStart)

        val afterWipe = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.ROOTFS, afterWipe.state.step)
        assertEquals(RootfsStatus.MISSING, afterWipe.state.rootfsStatus)
        assertEquals(0, afterWipe.state.progress)

        // Persisted EXTRACTING, but rootfs finished writing to disk before kill -> READY
        afterWipe.dispatch(SetupEvent.StartRootfsInstall)
        afterWipe.dispatch(SetupEvent.DownloadComplete("/cache/r.tar.gz"))
        val finishedOnDisk = SetupStateMachine(store, rootfsInstalled = { true })
        assertEquals(SetupStep.ROOTFS, finishedOnDisk.state.step)
        assertEquals(RootfsStatus.READY, finishedOnDisk.state.rootfsStatus)
        assertEquals(100, finishedOnDisk.state.progress)
    }

    @Test
    fun corruptedStoreValuesFallbackSafely() {
        val store = InMemorySetupStore()
        store.putString(SetupStateMachine.KEY_STEP, "BOGUS_STEP")
        store.putString(SetupStateMachine.KEY_ROOTFS_STATUS, "BOGUS_STATUS")
        store.putInt(SetupStateMachine.KEY_PROGRESS, 999)

        val machine = SetupStateMachine(store, rootfsInstalled = { false })
        assertEquals(SetupStep.WELCOME, machine.state.step)
        assertEquals(RootfsStatus.MISSING, machine.state.rootfsStatus)
        assertEquals(0, machine.state.progress)
    }
}
