package org.lodos.app.guest

import java.io.File
import org.lodos.app.lite.LiteMode
import org.lodos.app.lite.LiteProfile

/** Host-side paths (absolute). shmDir defaults to tmpDir; sdcard=null -> no /sdcard bind. */
data class GuestPaths(
    val nativeLibDir: String,
    val filesDir: String,
    val rootfsDir: String,
    val runDir: String,
    val tmpDir: String,
    val sdcard: String? = null,
    val shmDir: String? = null,
)

data class GuestSpec(
    val paths: GuestPaths,
    val cmd: List<String> = emptyList(),
    val lite: LiteProfile? = null,
    val extraEnv: Map<String, String> = emptyMap(),
    /** host -> guest */
    val extraBinds: List<Pair<String, String>> = emptyList(),
    val waylandDisplay: String = "wayland-0",
    val workDir: String = "/root",
    /** T048: true -> guest command is /sbin/lodos-init (PID1 of the guest tree); [cmd] ignored. */
    val lodosInit: Boolean = false,
)

/** Pure builder for the app-uid proot guest command (T046). No process spawn here. */
object GuestLaunch {
    const val PROOT = "libproot.so"
    const val LOADER = "libproot_loader.so"
    const val GUEST_RUN = "/run/lodos"
    const val GUEST_AUDIO_SOCKET = "$GUEST_RUN/audio.sock"
    const val GUEST_PULSE_SOCKET = "$GUEST_RUN/pulse/native.sock"
    const val LODOS_INIT = "/sbin/lodos-init"
    const val GUEST_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

    fun validate(spec: GuestSpec) {
        val p = spec.paths
        val all = listOfNotNull(p.nativeLibDir, p.filesDir, p.rootfsDir, p.runDir, p.tmpDir, p.sdcard, p.shmDir) +
            spec.extraBinds.flatMap { listOf(it.first, it.second) }
        all.forEach { require(it.startsWith("/")) { "relative path: '$it'" } }
        require(File(p.rootfsDir).isDirectory) { "rootfs missing: ${p.rootfsDir}" }
        require(effectiveCmd(spec).isNotEmpty()) { "empty cmd" }
    }

    fun effectiveCmd(spec: GuestSpec): List<String> = if (spec.lodosInit) listOf(LODOS_INIT) else spec.cmd

    fun prootPath(p: GuestPaths) = "${p.nativeLibDir}/$PROOT"
    fun loaderPath(p: GuestPaths) = "${p.nativeLibDir}/$LOADER"

    /** Env for the proot process itself (host side). */
    fun hostEnv(spec: GuestSpec): Map<String, String> = linkedMapOf(
        "PROOT_LOADER" to loaderPath(spec.paths),
        "PROOT_TMP_DIR" to spec.paths.tmpDir,
    )

    /** Clean guest env (passed via `env -i`); lite env merged, extraEnv wins. */
    fun guestEnv(spec: GuestSpec): Map<String, String> {
        val e = linkedMapOf(
            "HOME" to "/root",
            "PATH" to GUEST_PATH,
            "TERM" to "xterm",
            "LANG" to "C.UTF-8",
            "XDG_RUNTIME_DIR" to GUEST_RUN,
            "WAYLAND_DISPLAY" to spec.waylandDisplay,
            "PULSE_SERVER" to "unix:$GUEST_PULSE_SOCKET",
        )
        spec.lite?.let { e.putAll(LiteMode.toGuestEnv(it)) }
        if (spec.lite != null) e["LODOS_GSETTINGS_OVERRIDE"] = "$GUEST_SCHEMA_DIR/99-lodos.gschema.override"
        e.putAll(spec.extraEnv)
        return e
    }

    /** hostEnv + guestEnv (guest keys never shadow PROOT_*). */
    fun buildEnv(spec: GuestSpec): Map<String, String> {
        validate(spec)
        return LinkedHashMap(guestEnv(spec)).apply { putAll(hostEnv(spec)) }
    }

    fun buildArgv(spec: GuestSpec): List<String> {
        validate(spec)
        val p = spec.paths
        val a = mutableListOf(
            prootPath(p), "-0", "-r", p.rootfsDir,
            "--link2symlink", "--sysvipc", "--kill-on-exit",
            "-b", "/dev",
            "-b", "${p.shmDir ?: p.tmpDir}:/dev/shm",
            "-b", "/proc",
            "-b", "/sys",
            "-b", "${p.runDir}:$GUEST_RUN",
        )
        p.sdcard?.let { a += listOf("-b", "$it:/sdcard") }
        spec.extraBinds.forEach { (h, g) -> a += listOf("-b", "$h:$g") }
        spec.lite?.let { profile ->
            val override = File(p.filesDir, "lite/99-lodos.gschema.override")
            override.parentFile?.mkdirs()
            override.writeText(LiteMode.toGsettings(profile).joinToString("\n", postfix = "\n"))
            a += listOf("-b", "${override.absolutePath}:$GUEST_SCHEMA_DIR/99-lodos.gschema.override")
        }
        a += listOf("-w", spec.workDir, "/usr/bin/env", "-i")
        guestEnv(spec).forEach { (k, v) -> a += "$k=$v" }
        a += effectiveCmd(spec)
        return a
    }

    private const val GUEST_SCHEMA_DIR = "/usr/share/glib-2.0/schemas"
}
