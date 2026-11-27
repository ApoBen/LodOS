package org.lodos.app.lite

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LiteModeTest {
    private fun spec(ram: Int, cores: Int = 8, gpu: String = "Mali-G720") =
        DeviceSpec(ram, cores, 3000, "MediaTek MT6899", gpu, 4096)

    @Test fun ram4gbIsLow() = assertEquals(Tier.LOW, LiteMode.classify(spec(4096)))
    @Test fun ram6gbIsLow() = assertEquals(Tier.LOW, LiteMode.classify(spec(6144)))
    @Test fun d8400Mali720_12gbIsHigh() =
        assertEquals(Tier.HIGH, LiteMode.classify(spec(12288, gpu = "Mali-G720-Immortalis MC12")))
    @Test fun ram8gbIsMid() = assertEquals(Tier.MID, LiteMode.classify(spec(8192)))
    @Test fun fourCoresIsLow() = assertEquals(Tier.LOW, LiteMode.classify(spec(12288, cores = 4)))

    @Test fun weakGpuOn8gbIsLow() {
        assertEquals(Tier.LOW, LiteMode.classify(spec(8192, gpu = "Mali-G57 MC2")))
        assertEquals(Tier.LOW, LiteMode.classify(spec(8192, gpu = "Adreno (TM) 610")))
        assertEquals(Tier.LOW, LiteMode.classify(spec(8192, gpu = "Adreno (TM) 540")))
        assertEquals(Tier.MID, LiteMode.classify(spec(8192, gpu = "Adreno (TM) 660")))
        assertFalse(LiteMode.isWeakGpu("Mali-G710"))
    }

    @Test fun userForceOnOff() {
        val high = Tier.HIGH
        assertTrue(LiteMode.override(high, true).isLite)
        assertEquals(RootfsFlavor.MINIMAL, LiteMode.override(high, true).rootfsFlavor)
        assertFalse(LiteMode.override(Tier.LOW, false).isLite)
        assertEquals(LiteMode.profileFor(Tier.MID), LiteMode.override(Tier.MID, null))
        assertTrue(LiteMode.resolve(spec(4096), null).isLite)
    }

    @Test fun guestEnvOutput() {
        val lite = LiteMode.toGuestEnv(LiteMode.profileFor(Tier.LOW))
        assertEquals("1", lite["LODOS_LITE"])
        assertEquals("0", lite["GTK_ENABLE_ANIMATIONS"])
        assertEquals("minimal", lite["LODOS_ROOTFS_FLAVOR"])
        val full = LiteMode.toGuestEnv(LiteMode.profileFor(Tier.HIGH))
        assertEquals("0", full["LODOS_LITE"])
        assertNull(full["GTK_ENABLE_ANIMATIONS"])
    }

    @Test fun gsettingsOutput() {
        val lite = LiteMode.toGsettings(LiteMode.profileFor(Tier.LOW))
        assertEquals("org.gnome.desktop.interface enable-animations false", lite[0])
        assertTrue(lite.filter { it.startsWith("sm.puri.phosh") }.isEmpty())
        assertTrue(lite.any { it.startsWith("#") && "sm.puri.phosh" in it })
        assertEquals("org.gnome.desktop.interface enable-animations true",
            LiteMode.toGsettings(LiteMode.profileFor(Tier.HIGH))[0])
    }
}
