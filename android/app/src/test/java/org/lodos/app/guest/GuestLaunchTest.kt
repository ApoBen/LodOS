package org.lodos.app.guest

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.Tier

class GuestLaunchTest {
    private val rootfs = Files.createTempDirectory("rootfs").toFile().absolutePath

    private fun paths(sdcard: String? = null) =
        GuestPaths("/nlib", Files.createTempDirectory("files").toFile().absolutePath, rootfs, "/files/run", "/cache/tmp", sdcard)

    private fun spec(sdcard: String? = null, cmd: List<String> = listOf("/bin/sh", "-c", "id"), vararg env: Pair<String, String>) =
        GuestSpec(paths(sdcard), cmd, extraEnv = env.toMap())

    private fun List<String>.hasBind(b: String) = indices.any { this[it] == "-b" && getOrNull(it + 1) == b }

    @Test fun argvHasRootAndRootfs() {
        val a = GuestLaunch.buildArgv(spec())
        assertTrue("-0" in a)
        assertEquals(rootfs, a[a.indexOf("-r") + 1])
        assertTrue("--link2symlink" in a && "--kill-on-exit" in a)
    }

    @Test fun loaderFromNativeLibDir() {
        val s = spec()
        assertEquals("/nlib/libproot.so", GuestLaunch.buildArgv(s)[0])
        val e = GuestLaunch.buildEnv(s)
        assertEquals("/nlib/libproot_loader.so", e["PROOT_LOADER"])
        assertEquals("/cache/tmp", e["PROOT_TMP_DIR"])
    }

    @Test fun standardBinds() {
        val a = GuestLaunch.buildArgv(spec())
        listOf("/dev", "/proc", "/sys", "/cache/tmp:/dev/shm", "/files/run:/run/lodos").forEach {
            assertTrue("bind $it", a.hasBind(it))
        }
    }

    @Test fun shmDirOverridesTmp() {
        val a = GuestLaunch.buildArgv(GuestSpec(paths().copy(shmDir = "/files/shm"), listOf("true")))
        assertTrue(a.hasBind("/files/shm:/dev/shm"))
    }

    @Test fun sdcardOptional() {
        assertFalse(GuestLaunch.buildArgv(spec()).any { it.endsWith(":/sdcard") })
        assertTrue(GuestLaunch.buildArgv(spec("/storage/emulated/0")).hasBind("/storage/emulated/0:/sdcard"))
    }

    @Test fun extraBindsAdded() {
        val a = GuestLaunch.buildArgv(GuestSpec(paths(), listOf("true"), extraBinds = listOf("/a" to "/b")))
        assertTrue(a.hasBind("/a:/b"))
    }

    @Test fun defaultEnv() {
        val e = GuestLaunch.buildEnv(spec())
        assertEquals("/root", e["HOME"])
        assertEquals("C.UTF-8", e["LANG"])
        assertEquals("/run/lodos", e["XDG_RUNTIME_DIR"])
        assertEquals("wayland-0", e["WAYLAND_DISPLAY"])
        assertTrue(e["PATH"]!!.contains("/usr/bin"))
    }

    @Test fun liteEnvMerged() {
        val s = GuestSpec(paths(), listOf("true"), lite = LiteMode.profileFor(Tier.LOW))
        val e = GuestLaunch.buildEnv(s)
        assertEquals("1", e["LODOS_LITE"])
        assertEquals("0", e["GTK_ENABLE_ANIMATIONS"])
        assertTrue("LODOS_LITE=1" in GuestLaunch.buildArgv(s))
    }

    @Test fun extraEnvOverrides() {
        val s = GuestSpec(paths(), listOf("true"), lite = LiteMode.profileFor(Tier.LOW),
            extraEnv = mapOf("LANG" to "en_US.UTF-8", "LODOS_LITE" to "0", "PROOT_LOADER" to "/evil"))
        val e = GuestLaunch.buildEnv(s)
        assertEquals("en_US.UTF-8", e["LANG"])
        assertEquals("0", e["LODOS_LITE"])
        assertEquals("/nlib/libproot_loader.so", e["PROOT_LOADER"])
        val a = GuestLaunch.buildArgv(s)
        assertTrue("LANG=en_US.UTF-8" in a && "LANG=C.UTF-8" !in a)
    }

    @Test fun cmdAppendedLast() {
        val cmd = listOf("/bin/sh", "-c", "echo hi")
        val a = GuestLaunch.buildArgv(spec(cmd = cmd))
        assertEquals(cmd, a.takeLast(3))
        assertEquals(listOf("/usr/bin/env", "-i"), a.subList(a.indexOf("/usr/bin/env"), a.indexOf("/usr/bin/env") + 2))
    }

    @Test(expected = IllegalArgumentException::class)
    fun relativePathRejected() {
        GuestLaunch.buildArgv(GuestSpec(paths().copy(tmpDir = "cache/tmp"), listOf("true")))
    }

    @Test(expected = IllegalArgumentException::class)
    fun relativeSdcardRejected() {
        GuestLaunch.buildArgv(spec("sdcard"))
    }

    @Test(expected = IllegalArgumentException::class)
    fun missingRootfsRejected() {
        GuestLaunch.buildArgv(GuestSpec(paths().copy(rootfsDir = "$rootfs/nope"), listOf("true")))
    }

    @Test(expected = IllegalArgumentException::class)
    fun emptyCmdRejected() {
        GuestLaunch.buildEnv(GuestSpec(paths(), emptyList()))
    }
}
