package org.lodos.app.onboarding

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OnboardingChecksTest {
    @Test fun android30DoesNotWarnAboutPhantomProcesses() {
        val checks = OnboardingChecks.evaluate(30, true, "google")
        assertFalse(checks.any { it.id == "phantom_processes" })
    }

    @Test fun android31WarnsAboutPhantomProcesses() {
        val checks = OnboardingChecks.evaluate(31, true, "google")
        assertTrue(checks.any { it.id == "phantom_processes" })
    }

    @Test fun android34PointsAtModernDeveloperOption() {
        val check = OnboardingChecks.evaluate(34, true, "google").single()
        assertEquals("phantom_processes", check.id)
        assertEquals(ActionIntentKind.DEVELOPER_OPTIONS, check.actionIntent)
        assertEquals(org.lodos.app.R.string.onboarding_phantom_title_modern, check.titleRes)
    }

    @Test fun batteryOptimizationAddsRequestActionWhenNotExempted() {
        val check = OnboardingChecks.evaluate(30, false, "google").single()
        assertEquals("battery_optimization", check.id)
        assertEquals(ActionIntentKind.REQUEST_BATTERY_OPTIMIZATION, check.actionIntent)
    }

    @Test fun exemptBatteryDoesNotAddBatteryCheck() {
        assertFalse(OnboardingChecks.evaluate(31, true, "google").any { it.id == "battery_optimization" })
    }

    @Test fun xiaomiFamilyGetsOemGuidance() {
        val checks = OnboardingChecks.evaluate(30, true, "POCO")
        assertTrue(checks.any { it.id == "oem_background_limits" })
    }

    @Test fun pixelDoesNotGetOemGuidance() {
        assertFalse(OnboardingChecks.evaluate(34, true, "Google").any { it.id == "oem_background_limits" })
    }

    @Test fun readableDisabledPhantomMonitorChangesDetail() {
        val check = OnboardingChecks.evaluate(31, true, "pixel", phantomProcessMonitorValue = 0).single()
        assertEquals(org.lodos.app.R.string.onboarding_phantom_disabled_detail, check.detailRes)
    }
}
