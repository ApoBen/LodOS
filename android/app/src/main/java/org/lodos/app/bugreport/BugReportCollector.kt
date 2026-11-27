package org.lodos.app.bugreport

import android.app.ActivityManager
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import android.os.Build
import android.os.Process
import android.system.Os
import android.system.OsConstants
import android.view.Menu
import android.view.MenuItem
import android.widget.Toast
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Android glue for [BugReport]: gathers device info/logcat, shares zip, opens issue URL. */
object BugReportCollector {
    const val MENU_ID_REPORT = 0x10D05

    fun deviceInfo(ctx: Context): Map<String, String> {
        val m = LinkedHashMap<String, String>()
        m["manufacturer"] = Build.MANUFACTURER
        m["model"] = Build.MODEL
        m["device"] = Build.DEVICE
        if (Build.VERSION.SDK_INT >= 31) m["soc"] = "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}"
        m["android"] = Build.VERSION.RELEASE
        m["sdk"] = Build.VERSION.SDK_INT.toString()
        m["abi"] = Build.SUPPORTED_ABIS.joinToString(",")
        runCatching {
            val mi = ActivityManager.MemoryInfo()
            (ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(mi)
            m["ram"] = "${mi.totalMem / (1024 * 1024)} MiB total, ${mi.availMem / (1024 * 1024)} MiB avail"
        }
        runCatching { m["page size"] = Os.sysconf(OsConstants._SC_PAGESIZE).toString() }
        runCatching { gpuRenderer()?.let { m["gpu"] = it } }
        runCatching {
            @Suppress("DEPRECATION")
            val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            m["app version"] = "${pi.versionName} (${pi.longVersionCode})"
        }
        return m
    }

    /** GLES renderer string via a throwaway 1x1 pbuffer context; null on failure. */
    private fun gpuRenderer(): String? {
        val dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (dpy == EGL14.EGL_NO_DISPLAY) return null
        val ver = IntArray(2)
        if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) return null
        try {
            val cfgs = arrayOfNulls<EGLConfig>(1)
            val n = IntArray(1)
            val attr = intArrayOf(EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE)
            if (!EGL14.eglChooseConfig(dpy, attr, 0, cfgs, 0, 1, n, 0) || n[0] == 0) return null
            val ctx = EGL14.eglCreateContext(dpy, cfgs[0], EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
            val surf = EGL14.eglCreatePbufferSurface(dpy, cfgs[0],
                intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
            try {
                if (!EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) return null
                return "${GLES20.glGetString(GLES20.GL_RENDERER)} / ${GLES20.glGetString(GLES20.GL_VERSION)}"
            } finally {
                EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                EGL14.eglDestroySurface(dpy, surf)
                EGL14.eglDestroyContext(dpy, ctx)
            }
        } finally {
            EGL14.eglTerminate(dpy)
        }
    }

    /** Own-process logcat only (`logcat -d --pid`). */
    fun ownLogcat(): String = runCatching {
        val p = ProcessBuilder("logcat", "-d", "-v", "threadtime", "--pid", Process.myPid().toString())
            .redirectErrorStream(true).start()
        val out = p.inputStream.bufferedReader().readText()
        p.waitFor(5, TimeUnit.SECONDS)
        out
    }.getOrElse { "logcat failed: ${it.message}" }

    fun collect(ctx: Context, setupState: String): File {
        val logsDir = File(ctx.filesDir, "logs")
        val logFiles = logsDir.listFiles { f -> f.isFile && f.name.endsWith(".log") }?.toList().orEmpty()
        val outDir = File(ctx.cacheDir, "bugreports")
        outDir.listFiles()?.forEach { it.delete() } // keep only the latest report
        return BugReport.writeZip(
            BugReport.Input(deviceInfo(ctx), setupState, ownLogcat(), logFiles, System.currentTimeMillis()),
            outDir
        )
    }

    fun shareIntent(ctx: Context, zip: File): Intent {
        val uri = BugReportFileProvider.uriFor(ctx, zip)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/zip"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, "LodOS bug report")
            clipData = ClipData.newRawUri(zip.name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        return Intent.createChooser(send, "Share bug report")
    }

    fun issueIntent(zip: File): Intent {
        val report = BugReport.readReport(zip)
        val title = BugReport.defaultTitle(mapOf("model" to Build.MODEL, "sdk" to Build.VERSION.SDK_INT.toString()))
        val url = BugReport.issueUrl(title, BugReport.issueBody(report))
        return Intent(Intent.ACTION_VIEW, Uri.parse(url))
    }

    /** One-button flow: collect on a worker, then share zip and offer the prefilled issue. */
    fun reportProblem(activity: android.app.Activity, setupState: String) {
        Toast.makeText(activity, "Collecting logs…", Toast.LENGTH_SHORT).show()
        val ex = Executors.newSingleThreadExecutor()
        ex.execute {
            val result = runCatching { collect(activity.applicationContext, setupState) }
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                result.onSuccess { zip ->
                    android.app.AlertDialog.Builder(activity)
                        .setTitle("Bug report ready")
                        .setMessage("${zip.name}\n\nShare the zip, then open a GitHub issue (prefilled; attach the zip there).")
                        .setPositiveButton("Share zip") { _, _ -> safeStart(activity, shareIntent(activity, zip)) }
                        .setNeutralButton("Open issue") { _, _ -> safeStart(activity, issueIntent(zip)) }
                        .setNegativeButton("Close", null)
                        .show()
                }.onFailure {
                    Toast.makeText(activity, "Bug report failed: ${it.message}", Toast.LENGTH_LONG).show()
                }
            }
        }
        ex.shutdown()
    }

    private fun safeStart(activity: android.app.Activity, intent: Intent) {
        try {
            activity.startActivity(intent)
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(activity, "No app to handle this", Toast.LENGTH_LONG).show()
        }
    }

    // ---- menu helpers (for the future main activity) ----
    fun addMenuItem(menu: Menu): MenuItem = menu.add(Menu.NONE, MENU_ID_REPORT, Menu.NONE, "Report a problem")

    fun handleMenuItem(activity: android.app.Activity, item: MenuItem, setupState: String): Boolean {
        if (item.itemId != MENU_ID_REPORT) return false
        reportProblem(activity, setupState)
        return true
    }
}
