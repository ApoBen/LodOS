package org.lodos.app.onboarding

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.lodos.app.R

class OnboardingActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(24), dp(24), dp(24))
        }
        content.addView(TextView(this).apply {
            setText(R.string.onboarding_title)
            textSize = 24f
        })
        content.addView(TextView(this).apply {
            setText(R.string.onboarding_intro)
            textSize = 16f
            setPadding(0, dp(12), 0, dp(16))
        })

        val phantomValue = runCatching {
            Settings.Global.getInt(contentResolver, "settings_enable_monitor_phantom_procs")
        }.getOrNull()
        val checks = OnboardingChecks.evaluate(
            sdkInt = Build.VERSION.SDK_INT,
            isIgnoringBatteryOptimizations = Build.VERSION.SDK_INT < 23 ||
                getSystemService(android.os.PowerManager::class.java).isIgnoringBatteryOptimizations(packageName),
            manufacturer = Build.MANUFACTURER,
            phantomProcessMonitorValue = phantomValue,
        )
        if (checks.isEmpty()) {
            content.addView(TextView(this).apply { setText(R.string.onboarding_all_clear); textSize = 16f })
        }
        checks.forEach { check ->
            content.addView(TextView(this).apply {
                setText(check.titleRes)
                textSize = 18f
                setPadding(0, dp(12), 0, dp(4))
            })
            content.addView(TextView(this).apply {
                setText(check.detailRes)
                textSize = 14f
            })
            content.addView(Button(this).apply {
                setText(R.string.onboarding_open_settings)
                setOnClickListener { openAction(check.actionIntent) }
            })
        }
        content.addView(TextView(this).apply {
            setText(R.string.onboarding_advanced_tip)
            textSize = 13f
            setPadding(0, dp(20), 0, dp(12))
        })
        content.addView(Button(this).apply {
            setText(R.string.onboarding_done)
            setOnClickListener { finish() }
        })
        setContentView(ScrollView(this).apply { addView(content, ViewGroup.LayoutParams(-1, -2)) })
    }

    private fun openAction(kind: ActionIntentKind) {
        val requested = when (kind) {
            ActionIntentKind.REQUEST_BATTERY_OPTIMIZATION -> if (Build.VERSION.SDK_INT >= 23) {
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
            } else appDetailsIntent()
            ActionIntentKind.DEVELOPER_OPTIONS -> Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS)
            ActionIntentKind.OEM_AUTOSTART -> Intent("miui.intent.action.OP_AUTO_START").setClassName(
                "com.miui.securitycenter", "com.miui.permcenter.autostart.AutoStartManagementActivity"
            )
        }
        val intent = if (requested.resolveActivity(packageManager) != null) requested else appDetailsIntent()
        runCatching { startActivity(intent) }.onFailure { startActivity(appDetailsIntent()) }
    }

    private fun appDetailsIntent() = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName"))
    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
