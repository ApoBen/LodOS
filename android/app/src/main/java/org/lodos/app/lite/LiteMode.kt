package org.lodos.app.lite

/** Pure-JVM lite-mode decision core (T044). No Android deps here. */
data class DeviceSpec(
    val ramMb: Int,
    val cpuCores: Int,
    val maxFreqMhz: Int,
    val socModel: String,
    val gpuRenderer: String,
    val pageSize: Int,
)

enum class Tier { LOW, MID, HIGH }

enum class RootfsFlavor(val id: String) { MINIMAL("minimal"), FULL("full") }

data class LiteProfile(
    val animations: Boolean,
    val zramMb: Int,
    val rootfsFlavor: RootfsFlavor,
    val maxFps: Int,
    val compositorScale: Float,
    val phoshAnimations: Boolean,
) {
    val isLite: Boolean get() = !animations
}

object LiteMode {
    const val LOW_RAM_MB = 6144
    const val MID_RAM_MB = 8192
    const val LOW_CORES = 4

    // Mali-G5x/G6x (e.g. G52, G57, G68) and Adreno 5xx / 60x-61x.
    private val weakGpu = listOf(
        Regex("""mali-?g[56]\d\b""", RegexOption.IGNORE_CASE),
        Regex("""adreno\D*5\d\d\b""", RegexOption.IGNORE_CASE),
        Regex("""adreno\D*6[01]\d\b""", RegexOption.IGNORE_CASE),
    )

    fun isWeakGpu(renderer: String): Boolean = weakGpu.any { it.containsMatchIn(renderer) }

    fun classify(spec: DeviceSpec): Tier = when {
        spec.ramMb <= LOW_RAM_MB || spec.cpuCores <= LOW_CORES || isWeakGpu(spec.gpuRenderer) -> Tier.LOW
        spec.ramMb <= MID_RAM_MB -> Tier.MID
        else -> Tier.HIGH
    }

    // zramMb is advisory only: rootless => no zram (see docs/research/lite-mode.md).
    fun profileFor(tier: Tier): LiteProfile = when (tier) {
        Tier.LOW -> LiteProfile(false, 0, RootfsFlavor.MINIMAL, 30, 0.75f, false)
        Tier.MID -> LiteProfile(true, 0, RootfsFlavor.FULL, 60, 1.0f, false)
        Tier.HIGH -> LiteProfile(true, 0, RootfsFlavor.FULL, 120, 1.0f, true)
    }

    /** userForce: true = force lite (LOW profile), false = force full (HIGH profile), null = auto. */
    fun override(tier: Tier, userForce: Boolean?): LiteProfile = when (userForce) {
        true -> profileFor(Tier.LOW)
        false -> profileFor(Tier.HIGH)
        null -> profileFor(tier)
    }

    fun resolve(spec: DeviceSpec, userForce: Boolean?): LiteProfile = override(classify(spec), userForce)

    fun toGuestEnv(p: LiteProfile): Map<String, String> {
        val env = linkedMapOf(
            "LODOS_LITE" to if (p.isLite) "1" else "0",
            "LODOS_MAX_FPS" to p.maxFps.toString(),
            "LODOS_ROOTFS_FLAVOR" to p.rootfsFlavor.id,
            "LODOS_COMPOSITOR_SCALE" to p.compositorScale.toString(),
        )
        if (!p.animations) env["GTK_ENABLE_ANIMATIONS"] = "0"
        return env
    }

    fun toGsettings(p: LiteProfile): List<String> = listOf(
        "org.gnome.desktop.interface enable-animations ${p.animations}",
        // No known stable sm.puri.phosh animation key; phosh follows enable-animations.
        "# sm.puri.phosh: no dedicated animation key (phoshAnimations=${p.phoshAnimations}); relies on org.gnome.desktop.interface enable-animations",
    )
}
