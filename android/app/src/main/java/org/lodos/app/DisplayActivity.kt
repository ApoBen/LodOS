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
            spawnShmFill(runDir, sockPath, bench)
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
