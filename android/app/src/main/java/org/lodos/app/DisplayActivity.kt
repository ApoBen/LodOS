package org.lodos.app

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import java.io.File
import java.util.concurrent.TimeUnit
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestPaths
import org.lodos.app.guest.GuestSpec

/**
 * T022 (S2 v0): Fullscreen Wayland SurfaceView host.
 * Starts liblodos_wl.so JNI Wayland server on <filesDir>/run/wayland-0 with the SurfaceView's Surface,
 * and spawns the staged wl_shm green test client (libshmfill.so) unless `-e autoclient false` is given.
 */
class DisplayActivity : Activity(), SurfaceHolder.Callback {

    private var clientStarted = false
    @Volatile
    private var clientProc: Process? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)

        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val runDir = File(filesDir, "run").apply { mkdirs() }
        val sockPath = File(runDir, "wayland-0").absolutePath
        Log.i(TAG, "surfaceCreated -> native start($sockPath)")
        start(sockPath, holder.surface)

        val autoClient = intent?.getBooleanExtra("autoclient", true) ?: true
        if (autoClient && !clientStarted) {
            clientStarted = true
            val bench = intent?.getIntExtra("bench", 0) ?: 0
            if (intent?.getStringExtra("client") == "guest") {
                spawnGuest(runDir)
            } else {
                spawnShmFill(runDir, sockPath, bench)
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val sockPath = File(File(filesDir, "run"), "wayland-0").absolutePath
        Log.i(TAG, "surfaceChanged ${width}x${height} fmt=$format")
        start(sockPath, holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceDestroyed -> releaseWindow()")
        releaseWindow()
    }

    override fun onDestroy() {
        super.onDestroy()
        clientProc?.let { p ->
            p.destroy()
            if (!p.waitFor(500, TimeUnit.MILLISECONDS)) {
                p.destroyForcibly()
            }
        }
        clientProc = null
        stop()
    }

    private fun spawnShmFill(runDir: File, sockPath: String, benchFrames: Int) {
        val bin = File(applicationInfo.nativeLibraryDir, "libshmfill.so")
        if (!bin.exists()) {
            Log.e(TAG, "missing ${bin.absolutePath}")
            return
        }
        Thread({
            try {
                val cmd = mutableListOf(bin.absolutePath)
                if (benchFrames > 0) {
                    cmd += listOf("--bench", benchFrames.toString())
                }
                val logFile = File(cacheDir, "shmfill.log")
                val pb = ProcessBuilder(cmd)
                pb.environment()["WAYLAND_DISPLAY"] = sockPath
                pb.environment()["XDG_RUNTIME_DIR"] = runDir.absolutePath
                pb.redirectErrorStream(true)
                pb.redirectOutput(logFile)
                Log.i(TAG, "starting client ${bin.absolutePath} WAYLAND_DISPLAY=$sockPath")
                val p = pb.start()
                clientProc = p
                val rc = p.waitFor()
                val out = logFile.takeIf { it.exists() }?.readText()?.trim() ?: ""
                Log.i(TAG, "client exited rc=$rc out=${out.replace('\n', '|')}")
            } catch (t: Throwable) {
                Log.e(TAG, "client failed: ${t.javaClass.simpleName}: ${t.message}")
            }
        }, "lodos-shmfill").start()
    }

    /** T028: run a Debian wl_shm client at the app uid through the same socket as local clients. */
    private fun spawnGuest(runDir: File) {
        Thread({
            try {
                val rootfs = File(filesDir, "rootfs")
                val prootTmp = File(cacheDir, "proot-tmp").apply { mkdirs() }
                val shmDir = File(filesDir, "shm").apply { mkdirs() }
                val paths = GuestPaths(
                    nativeLibDir = applicationInfo.nativeLibraryDir,
                    filesDir = filesDir.absolutePath,
                    rootfsDir = rootfs.absolutePath,
                    runDir = runDir.absolutePath,
                    tmpDir = prootTmp.absolutePath,
                    shmDir = shmDir.absolutePath,
                )
                val spec = GuestSpec(
                    paths = paths,
                    cmd = listOf("/bin/sh", "-lc", """
                        set -e
                        mkdir -p /etc/apt/apt.conf.d /etc/dpkg/dpkg.cfg.d
                        printf '%s\\n' 'APT::Sandbox::User \"root\";' 'APT::Install-Recommends \"false\";' > /etc/apt/apt.conf.d/01lodos
                        printf '%s\\n' 'force-unsafe-io' > /etc/dpkg/dpkg.cfg.d/01lodos
                        if [ -L /etc/resolv.conf ] || ! grep -q '^nameserver' /etc/resolv.conf 2>/dev/null; then
                          rm -f /etc/resolv.conf; printf 'nameserver 8.8.8.8\\nnameserver 1.1.1.1\\n' > /etc/resolv.conf
                        fi
                        if ! command -v weston-simple-shm >/dev/null 2>&1; then apt-get update -q && apt-get install -y -q --no-install-recommends weston; fi
                        timeout 5s weston-simple-shm
                    """.trimIndent()),
                    workDir = "/root",
                )
                val argv = listOf(GuestLaunch.prootPath(paths)) + GuestLaunch.buildArgv(spec).drop(1)
                val pb = ProcessBuilder(argv)
                pb.environment().putAll(GuestLaunch.buildEnv(spec))
                val logFile = File(cacheDir, "guest-wl-client.log")
                pb.redirectErrorStream(true).redirectOutput(logFile)
                Log.i(TAG, "starting Debian guest wl_shm client rootfs=${rootfs.absolutePath} display=/run/lodos/wayland-0")
                val p = pb.start()
                clientProc = p
                val rc = p.waitFor()
                Log.i(TAG, "guest client exited rc=$rc out=${logFile.takeIf { it.exists() }?.readText()?.trim()?.replace('\n', '|') ?: ""}")
            } catch (t: Throwable) {
                Log.e(TAG, "guest client failed: ${t.javaClass.simpleName}: ${t.message}")
            }
        }, "lodos-guest-wl-client").start()
    }

    private external fun start(socketPath: String, surface: Surface)
    private external fun releaseWindow()
    private external fun stop()

    companion object {
        private const val TAG = "LODOS_DISPLAY"

        init {
            System.loadLibrary("lodos_wl")
        }
    }
}
