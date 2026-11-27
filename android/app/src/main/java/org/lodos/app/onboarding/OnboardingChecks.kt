package org.lodos.app.onboarding

import org.lodos.app.R

enum class CheckSeverity { WARNING, INFO }

enum class ActionIntentKind { REQUEST_BATTERY_OPTIMIZATION, DEVELOPER_OPTIONS, OEM_AUTOSTART }

data class Check(
    val id: String,
    val severity: CheckSeverity,
    val titleRes: Int,
    val actionIntent: ActionIntentKind,
    val detailRes: Int,
)

/** Pure device-risk rules. A null phantom setting means the value was unavailable. */
object OnboardingChecks {
    fun evaluate(
        sdkInt: Int,
        isIgnoringBatteryOptimizations: Boolean,
        manufacturer: String,
        phantomProcessMonitorValue: Int? = null,
    ): List<Check> = buildList {
        if (sdkInt >= 31) {
            add(Check(
                id = "phantom_processes",
                severity = CheckSeverity.WARNING,
                titleRes = if (sdkInt >= 34) R.string.onboarding_phantom_title_modern else R.string.onboarding_phantom_title,
                actionIntent = ActionIntentKind.DEVELOPER_OPTIONS,
                detailRes = if (phantomProcessMonitorValue == 0) R.string.onboarding_phantom_disabled_detail else R.string.onboarding_phantom_detail,
            ))
        }
        if (!isIgnoringBatteryOptimizations) {
            add(Check(
                id = "battery_optimization",
                severity = CheckSeverity.WARNING,
                titleRes = R.string.onboarding_battery_title,
                actionIntent = ActionIntentKind.REQUEST_BATTERY_OPTIMIZATION,
                detailRes = R.string.onboarding_battery_detail,
            ))
        }
        if (manufacturer.lowercase().let { it.contains("xiaomi") || it.contains("poco") || it.contains("redmi") }) {
            add(Check(
                id = "oem_background_limits",
                severity = CheckSeverity.INFO,
                titleRes = R.string.onboarding_oem_title,
                actionIntent = ActionIntentKind.OEM_AUTOSTART,
                detailRes = R.string.onboarding_oem_detail,
            ))
        }
    }
}
