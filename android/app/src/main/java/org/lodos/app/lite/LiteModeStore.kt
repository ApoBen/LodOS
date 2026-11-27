package org.lodos.app.lite

import android.content.Context

/** SharedPreferences glue for user lite-mode override. null = auto. */
class LiteModeStore(context: Context) {
    private val prefs = context.getSharedPreferences("lodos_lite", Context.MODE_PRIVATE)

    var userForce: Boolean?
        get() = if (prefs.contains(KEY)) prefs.getBoolean(KEY, false) else null
        set(v) {
            prefs.edit().apply { if (v == null) remove(KEY) else putBoolean(KEY, v) }.apply()
        }

    private companion object { const val KEY = "force_lite" }
}
