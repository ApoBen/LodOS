package org.lodos.app

import android.app.Activity
import android.os.Bundle
import android.os.Process
import android.system.Os
import android.util.Log
import java.io.File
import org.lodos.app.guest.GuestLaunch
import org.lodos.app.guest.GuestPaths
import org.lodos.app.guest.GuestSpec
import java.util.concurrent.TimeUnit

/**
 * T017/T020: app-uid probes (R8/R10/R13).
 * Default (T017): adb shell am start -n org.lodos.app/.ProbeActivity
 * Rootfs  (T020): adb shell am start -n org.lodos.app/.ProbeActivity -e probe rootfs [-e tarball /path/to/tar.gz]
 */
class ProbeActivity : Activity() {

    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (started) return
        started = true
        val mode = intent?.getStringExtra("probe") ?: intent?.getStringExtra("action") ?: "default"
        if (mode == "rootfs") {
            val tarball = intent?.getStringExtra("tarball") ?: DEFAULT_TARBALL
            val rootfsName = intent?.getStringExtra("rootfs") ?: "rootfs"
            Thread({ runRootfsProbe(tarball, rootfsName) }, "lodos-rfs-probe").start()
        } else {
            Thread({ runProbe() }, "lodos-probe").start()
        }
    }

    private external fun probeDmaHeapNative(): String
    private external fun extractRootfsNative(src: String, dst: String): String

    private fun log(msg: String) = Log.i(TAG, msg)

    private fun tail(s: String, n: Int = 300) = s.trim().replace('\n', '|').takeLast(n)

    // ---- T017 default probe ----

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
        val wlBin = File(nlib, "liblodos_wl_bin.so").takeIf { it.exists() } ?: File(nlib, "liblodos_wl.so")
        val p = ProcessBuilder(wlBin.absolutePath, sock.absolutePath)
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

    // ---- T020 rootfs + apt + dma_heap probe ----

    private data class CmdRes(val rc: Int, val out: String, val err: String, val ms: Long, val timedOut: Boolean = false)

    private fun runRootfsProbe(tarballArg: String, rootfsName: String) {
        val tAll = System.currentTimeMillis()
        try {
            log("uid=${Process.myUid()}")
            val nlib = applicationInfo.nativeLibraryDir
            log("nativeLibraryDir=$nlib")

            // 1. /dev/dma_heap/system probe at app uid (R8/R11)
            val dma = try {
                probeDmaHeapNative()
            } catch (t: Throwable) {
                "fail exc=${t.javaClass.simpleName}:${t.message}"
            }
            log("DMAHEAP $dma")

            require(rootfsName == "rootfs" || rootfsName == "rootfs2") { "invalid rootfs name" }
            val rootfs = File(filesDir, rootfsName)
            val prootTmp = File(cacheDir, "proot-tmp").apply { mkdirs() }
            val shmDir = File(filesDir, "shm").apply { mkdirs() }
            try {
                Os.chmod(shmDir.absolutePath, 0x3ff) // 01777
            } catch (_: Throwable) {
            }

            // 2. Extract rootfs (idempotent: skip if <filesDir>/rootfs/etc/debian_version exists)
            if (!ensureRootfs(nlib, prootTmp, rootfs, tarballArg)) return

            // 3. Guest setup (apt/dpkg/dns/groups/policy-rc.d, same as scripts/guest-setup.sh)
            val setupCmd = """
                set -e
                mkdir -p /etc/apt/apt.conf.d /etc/dpkg/dpkg.cfg.d
                printf '%s\n' 'APT::Sandbox::User "root";' 'Acquire::Retries "3";' 'APT::Install-Recommends "false";' > /etc/apt/apt.conf.d/01lodos
                printf '%s\n' 'force-unsafe-io' > /etc/dpkg/dpkg.cfg.d/01lodos
                for g in aid_inet:3003 aid_net_raw:3004 aid_net_admin:3005 aid_net_bw_stats:3006 aid_net_bw_acct:3007 aid_everybody:9997; do
                  n=${'$'}{g%%:*}; id=${'$'}{g##*:}
                  grep -q "^${'$'}n:" /etc/group || echo "${'$'}n:x:${'$'}id:root" >> /etc/group
                done
                if [ -L /etc/resolv.conf ] || ! grep -q '^nameserver' /etc/resolv.conf 2>/dev/null; then
                  rm -f /etc/resolv.conf
                  printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
                fi
                printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
                chmod 755 /usr/sbin/policy-rc.d
                echo cfg-ok
            """.trimIndent()
            val rSetup = runGuest(nlib, prootTmp, shmDir, rootfs, "setup", setupCmd, 60)
            if (rSetup.rc == 0) log("RFS setup ok (${rSetup.ms}ms)")
            else {
                log("RFS setup fail rc=${rSetup.rc} out=${tail(rSetup.out, 150)} err=${tail(rSetup.err)}")
                return
            }

            // 4. cat /etc/debian_version
            val rVer = runGuest(nlib, prootTmp, shmDir, rootfs, "ver", "cat /etc/debian_version", 30)
            if (rVer.rc == 0 && rVer.out.trim().isNotEmpty()) {
                log("RFS debian_version ok ${tail(rVer.out, 80)} (${rVer.ms}ms)")
            } else {
                log("RFS debian_version fail rc=${rVer.rc} out=${tail(rVer.out, 100)} err=${tail(rVer.err)}")
            }

            // 5. id -u
            val rId = runGuest(nlib, prootTmp, shmDir, rootfs, "id", "id -u", 30)
            if (rId.rc == 0 && rId.out.trim() == "0") {
                log("RFS id ok uid=${rId.out.trim()} (${rId.ms}ms)")
            } else {
                log("RFS id fail rc=${rId.rc} out=${tail(rId.out, 80)} err=${tail(rId.err)}")
            }

            // 6. apt-get update
            val rUpd = runGuest(nlib, prootTmp, shmDir, rootfs, "apt-update", "apt-get update -q", 300)
            if (rUpd.rc == 0) {
                log("RFS apt-update ok (${rUpd.ms}ms)")
            } else {
                log("RFS apt-update fail rc=${rUpd.rc} (${rUpd.ms}ms) out=${tail(rUpd.out, 200)} err=${tail(rUpd.err)}")
                return
            }

            // 7. apt-get install -y --no-install-recommends hello
            val rIns = runGuest(
                nlib, prootTmp, shmDir, rootfs, "apt-install",
                "apt-get install -y -q --no-install-recommends hello",
                300
            )
            if (rIns.rc == 0) {
                log("RFS apt-install ok (${rIns.ms}ms)")
            } else {
                log("RFS apt-install fail rc=${rIns.rc} (${rIns.ms}ms) out=${tail(rIns.out, 200)} err=${tail(rIns.err)}")
                return
            }

            // 8. hello
            val rHello = runGuest(nlib, prootTmp, shmDir, rootfs, "hello", "hello", 30)
            if (rHello.rc == 0 && rHello.out.contains("Hello, world")) {
                log("RFS hello ok ${tail(rHello.out, 120)} (${rHello.ms}ms)")
            } else {
                log("RFS hello fail rc=${rHello.rc} out=${tail(rHello.out, 150)} err=${tail(rHello.err)}")
            }
        } catch (t: Throwable) {
            log("RFS EXC ${t.javaClass.simpleName}: ${t.message}")
        } finally {
            log("RFS_DONE total=${System.currentTimeMillis() - tAll}ms")
        }
    }

    private fun ensureRootfs(nlib: String, prootTmp: File, rootfs: File, tarballArg: String): Boolean {
        val verFile = File(rootfs, "etc/debian_version")
        val shFile = File(rootfs, "bin/sh")
        if (verFile.exists() && shFile.exists()) {
            log("RFS extract ok skipped (exists)")
            return true
        }

        val candidates = listOf(
            File(tarballArg),
            File(cacheDir, "rootfs.tar.gz"),
            File(filesDir, "rootfs.tar.gz")
        )
        val diag = mutableListOf<String>()
        var src: File? = null
        for (c in candidates) {
            try {
                c.inputStream().use { it.read() }
                src = c
                diag.add("${c.absolutePath}:readable")
                break
            } catch (t: Throwable) {
                diag.add("${c.absolutePath}:${t.javaClass.simpleName}(${t.message?.take(60)})")
            }
        }
        if (src == null) {
            log("RFS extract fail no readable tarball: ${diag.joinToString("; ")}")
            return false
        }
        log("RFS tarball src=${src.absolutePath} size=${src.length()} (${diag.joinToString("; ")})")

        val tmpDir = File(filesDir, "${rootfs.name}.tmp")
        cleanTree(tmpDir)
        cleanTree(rootfs)
        tmpDir.mkdirs()

        val t0 = System.currentTimeMillis()
        val summary = try {
            extractRootfsNative(src.absolutePath, tmpDir.absolutePath)
        } catch (t: Throwable) {
            log("RFS extract fail native ${t.javaClass.simpleName}:${t.message}")
            return false
        }
        val ms = System.currentTimeMillis() - t0
        val okFiles = File(tmpDir, "bin/sh").exists() && File(tmpDir, "etc/debian_version").exists()
        if (!okFiles) {
            log("RFS extract fail native (${ms}ms) missing bin/sh or debian_version summary=$summary")
            return false
        }
        if (!tmpDir.renameTo(rootfs)) {
            log("RFS extract fail native rename ${tmpDir.absolutePath} -> ${rootfs.absolutePath}")
            return false
        }
        // Clean staged copy in cacheDir/filesDir if used (never delete /data/local/tmp)
        if (src.parentFile == cacheDir || src.parentFile == filesDir) {
            src.delete()
        }
        log("RFS extract ok native $ms summary=$summary")
        return true
    }

    private fun cleanTree(dir: File) {
        if (!dir.exists()) return
        try {
            ProcessBuilder("/system/bin/chmod", "-R", "u+rwx", dir.absolutePath)
                .redirectErrorStream(true).start().waitFor(30, TimeUnit.SECONDS)
        } catch (_: Throwable) {
        }
        try {
            ProcessBuilder("/system/bin/rm", "-rf", dir.absolutePath)
                .redirectErrorStream(true).start().waitFor(60, TimeUnit.SECONDS)
        } catch (_: Throwable) {
            dir.deleteRecursively()
        }
    }

    private fun runGuest(
        nlib: String,
        prootTmp: File,
        shmDir: File,
        rootfs: File,
        step: String,
        cmd: String,
        timeoutSec: Long
    ): CmdRes {
        val outF = File(cacheDir, "rfs-$step.out")
        val errF = File(cacheDir, "rfs-$step.err")
        val spec = GuestSpec(
            paths = GuestPaths(
                nativeLibDir = nlib,
                filesDir = filesDir.absolutePath,
                rootfsDir = rootfs.absolutePath,
                runDir = File(filesDir, "run").apply { mkdirs() }.absolutePath,
                tmpDir = prootTmp.absolutePath,
                shmDir = shmDir.absolutePath,
            ),
            cmd = listOf("/bin/sh", "-c", cmd),
            extraEnv = mapOf("DEBIAN_FRONTEND" to "noninteractive"),
        )
        val pb = ProcessBuilder(GuestLaunch.buildArgv(spec))
        pb.environment().putAll(GuestLaunch.hostEnv(spec))
        val t0 = System.currentTimeMillis()
        val p = pb.redirectOutput(outF).redirectError(errF).start()
        val ok = p.waitFor(timeoutSec, TimeUnit.SECONDS)
        val ms = System.currentTimeMillis() - t0
        if (!ok) {
            p.destroyForcibly()
            val out = outF.takeIf { it.exists() }?.readText() ?: ""
            val err = errF.takeIf { it.exists() }?.readText() ?: ""
            return CmdRes(-1, out, "timeout(${timeoutSec}s) $err", ms, timedOut = true)
        }
        val rc = p.exitValue()
        val out = outF.takeIf { it.exists() }?.readText() ?: ""
        val err = errF.takeIf { it.exists() }?.readText() ?: ""
        return CmdRes(rc, out, err, ms)
    }

    companion object {
        private const val TAG = "LODOS_PROBE"
        private const val DEFAULT_TARBALL = "/data/local/tmp/lodos/debian-trixie-arm64.tar.gz"

        init {
            System.loadLibrary("lodos_native")
            System.loadLibrary("lodos_rootfs")
        }
    }
}
