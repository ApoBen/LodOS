package org.lodos.app.settings

import android.content.Context

data class LodosSettings(
    val liteMode: String = LITE_AUTO,
    val fgPreset: String = FG_OFF,
    val fgSharpness: Float = 0.2f,
) {
    companion object {
        const val LITE_AUTO = "auto"
        const val LITE_ON = "on"
        const val LITE_OFF = "off"
        const val FG_OFF = "off"
        const val FG_QUALITY = "quality"
        const val FG_BALANCED = "balanced"
        const val FG_PERFORMANCE = "performance"
    }
}

/** SharedPreferences persistence for the user facing graphics and performance settings. */
class LodosSettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("lodos_settings", Context.MODE_PRIVATE)

    fun read() = LodosSettings(
        liteMode = prefs.getString(KEY_LITE, LodosSettings.LITE_AUTO) ?: LodosSettings.LITE_AUTO,
        fgPreset = prefs.getString(KEY_PRESET, LodosSettings.FG_OFF) ?: LodosSettings.FG_OFF,
        fgSharpness = prefs.getFloat(KEY_SHARPNESS, 0.2f).coerceIn(0f, 1f),
    )

    fun write(settings: LodosSettings) {
        prefs.edit()
            .putString(KEY_LITE, settings.liteMode)
            .putString(KEY_PRESET, settings.fgPreset)
            .putFloat(KEY_SHARPNESS, settings.fgSharpness.coerceIn(0f, 1f))
            .apply()
    }

    private companion object {
        const val KEY_LITE = "lite_mode"
        const val KEY_PRESET = "fg_preset"
        const val KEY_SHARPNESS = "fg_sharpness"
    }
}

