package org.lodos.app.guest

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.Tier

class GuestLaunchLiteTest {
    private val rootfs = Files.createTempDirectory("lite-rootfs").toFile().absolutePath
    private val paths get() = GuestPaths("/nlib", Files.createTempDirectory("lite-files").toFile().absolutePath,
        rootfs, "/run", "/tmp")

    @Test fun fullAndLiteHaveDifferentGuestEnvironments() {
        val full = GuestLaunch.guestEnv(GuestSpec(paths, listOf("true"), lite = LiteMode.profileFor(Tier.HIGH)))
        val lite = GuestLaunch.guestEnv(GuestSpec(paths, listOf("true"), lite = LiteMode.profileFor(Tier.LOW)))
        assertEquals("0", full["LODOS_LITE"])
        assertEquals("1", lite["LODOS_LITE"])
        assertNotEquals(full["LODOS_MAX_FPS"], lite["LODOS_MAX_FPS"])
        assertFalse(full.containsKey("GTK_ENABLE_ANIMATIONS"))
        assertEquals("0", lite["GTK_ENABLE_ANIMATIONS"])
    }

    @Test fun liteWritesAnimationOverrideAndBindsIt() {
        val p = paths
        val argv = GuestLaunch.buildArgv(GuestSpec(p, listOf("true"), lite = LiteMode.profileFor(Tier.LOW)))
        val file = java.io.File(p.filesDir, "lite/99-lodos.gschema.override")
        assertTrue(file.readText().contains("org.gnome.desktop.interface enable-animations false"))
        assertTrue(argv.any { it == "${file.absolutePath}:/usr/share/glib-2.0/schemas/99-lodos.gschema.override" })
    }

    @Test fun forcedOnSelectsLowProfile() {
        val forced = LiteMode.override(Tier.HIGH, true)
        assertTrue(forced.isLite)
        assertEquals(30, forced.maxFps)
    }

    @Test fun forcedOffSelectsFullProfile() {
        val forced = LiteMode.override(Tier.LOW, false)
        assertFalse(forced.isLite)
        assertEquals(120, forced.maxFps)
    }

    @Test fun nullProfileLeavesArgvUnchanged() {
        val p = paths
        assertEquals(GuestLaunch.buildArgv(GuestSpec(p, listOf("true"))),
            GuestLaunch.buildArgv(GuestSpec(p, listOf("true"), lite = null)))
    }
}
