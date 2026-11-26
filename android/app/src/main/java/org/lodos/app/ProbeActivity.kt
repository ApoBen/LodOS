package org.lodos.app

import android.app.Activity
import android.os.Bundle
import android.os.Process
import android.util.Log
import java.io.File
import java.util.concurrent.TimeUnit

/** T017: app-uid probe (R8/R10/R13). adb shell am start -n org.lodos.app/.ProbeActivity ; logcat -s LODOS_PROBE */
class ProbeActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Thread({ runProbe() }, "lodos-probe").start()
    }

    private fun log(msg: String) = Log.i(TAG, msg)

    private fun tail(s: String, n: Int = 300) = s.trim().replace('\n', '|').takeLast(n)

    private fun runProbe() {
        try {
            log("uid=${Process.myUid()}")
            val nlib = applicationInfo.nativeLibraryDir
            log("nativeLibraryDir=$nlib files=${File(nlib).list()?.sorted()}")
            probePathSocket(nlib)
            probeProot(nlib)
        } catch (t: Throwable) {
            log("EXC ${t.javaClass.simpleName}: ${t.message}")
        } finally {
            log("PROBE_DONE")
        }
    }

    private fun probePathSocket(nlib: String) {
        val run = File(filesDir, "run").apply { mkdirs() }
        val sock = File(run, "wayland-0")
        sock.delete()
        val errFile = File(cacheDir, "lodos-wl.err")
        val p = ProcessBuilder("$nlib/liblodos_wl.so", sock.absolutePath)
            .redirectErrorStream(true)
            .redirectOutput(errFile)
            .start()
        try {
            Thread.sleep(1000)
            val out = errFile.takeIf { it.exists() }?.readText() ?: ""
            if (sock.exists()) log("PATHSOCK ok ${sock.absolutePath} | ${tail(out, 200)}")
            else log("PATHSOCK fail alive=${p.isAlive} ${tail(out)}")
        } finally {
            p.destroy()
            if (!p.waitFor(2, TimeUnit.SECONDS)) p.destroyForcibly()
            sock.delete()
        }
    }

    private fun probeProot(nlib: String) {
        val pb = ProcessBuilder("$nlib/libproot.so", "-0", "/system/bin/id")
        pb.environment()["PROOT_LOADER"] = "$nlib/libproot_loader.so"
        pb.environment()["PROOT_TMP_DIR"] = cacheDir.absolutePath
        val outF = File(cacheDir, "proot.out")
        val errF = File(cacheDir, "proot.err")
        val p = pb.redirectOutput(outF).redirectError(errF).start()
        if (!p.waitFor(20, TimeUnit.SECONDS)) {
            p.destroyForcibly()
            log("PROOT fail rc=timeout ${tail(errF.readText())}")
            return
        }
        val rc = p.exitValue()
        val out = outF.readText()
        val err = errF.readText()
        if (rc == 0 && out.contains("uid=0")) log("PROOT ok ${tail(out)}${if (err.isNotBlank()) " | err: " + tail(err, 200) else ""}")
        else log("PROOT fail rc=$rc out=${tail(out, 150)} ${tail(err)}")
    }

    companion object {
        private const val TAG = "LODOS_PROBE"
    }
}
