package org.lodos.app.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.lodos.app.lite.Tier

class SettingsEnvTest {
    @Test fun defaults() {
        val settings = LodosSettings()
        val env = SettingsEnv.toEnv(settings, Tier.HIGH)
        assertEquals("auto", settings.liteMode)
        assertEquals("off", settings.fgPreset)
        assertEquals(0.2f, settings.fgSharpness)
        assertEquals("0", env["LODOS_LITE"])
        assertFalse(env.keys.any { it.startsWith("LODOS_FG") })
    }

    @Test fun fgOffOmitsUpscaleKeys() {
        assertFalse(SettingsEnv.toEnv(LodosSettings(fgPreset = "off"), Tier.HIGH).keys.any { it.startsWith("LODOS_FG") })
    }

    @Test fun eachPresetIsPassedThrough() {
        listOf("quality", "balanced", "performance").forEach { preset ->
            val env = SettingsEnv.toEnv(LodosSettings(fgPreset = preset), Tier.MID)
            assertEquals("1", env["LODOS_FG"])
            assertEquals(preset, env["LODOS_FG_PRESET"])
            assertEquals("0.2", env["LODOS_FG_SHARPNESS"])
        }
    }

    @Test fun sharpnessIsClamped() {
        assertEquals("0.0", SettingsEnv.toEnv(LodosSettings(fgPreset = "quality", fgSharpness = -2f), Tier.HIGH)["LODOS_FG_SHARPNESS"])
        assertEquals("1.0", SettingsEnv.toEnv(LodosSettings(fgPreset = "quality", fgSharpness = 2f), Tier.HIGH)["LODOS_FG_SHARPNESS"])
    }

    @Test fun liteOnAndOff() {
        assertEquals("1", SettingsEnv.toEnv(LodosSettings(liteMode = "on"), Tier.HIGH)["LODOS_LITE"])
        assertEquals("0", SettingsEnv.toEnv(LodosSettings(liteMode = "off"), Tier.LOW)["LODOS_LITE"])
    }

    @Test fun liteAutoUsesDeviceTier() {
        assertEquals("1", SettingsEnv.toEnv(LodosSettings(liteMode = "auto"), Tier.LOW)["LODOS_LITE"])
        assertEquals("0", SettingsEnv.toEnv(LodosSettings(liteMode = "auto"), Tier.HIGH)["LODOS_LITE"])
    }
}
