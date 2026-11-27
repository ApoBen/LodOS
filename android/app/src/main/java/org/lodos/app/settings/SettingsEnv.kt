package org.lodos.app.settings

import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.LiteProfile
import org.lodos.app.lite.Tier

/** Pure mapping from persisted choices to guest environment variables. */
object SettingsEnv {
    fun liteProfile(settings: LodosSettings, liteTier: Tier): LiteProfile = when (settings.liteMode) {
        LodosSettings.LITE_ON -> LiteMode.profileFor(Tier.LOW)
        LodosSettings.LITE_OFF -> LiteMode.profileFor(Tier.HIGH)
        else -> LiteMode.profileFor(liteTier)
    }

    fun toEnv(settings: LodosSettings, liteTier: Tier): Map<String, String> = buildMap {
        putAll(LiteMode.toGuestEnv(liteProfile(settings, liteTier)))
        when (settings.fgPreset) {
            LodosSettings.FG_QUALITY, LodosSettings.FG_BALANCED, LodosSettings.FG_PERFORMANCE -> {
                put("LODOS_FG", "1")
                put("LODOS_FG_PRESET", settings.fgPreset)
                put("LODOS_FG_SHARPNESS", settings.fgSharpness.coerceIn(0f, 1f).toString())
            }
        }
    }
}
