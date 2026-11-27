package org.lodos.app.session

import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** filesDir/logs/guest-<ts>.log, keeps newest [keep] files (T048). Pure JVM. */
class LogRotator(val dir: File, val keep: Int = KEEP, val prefix: String = PREFIX) {
    companion object {
        const val PREFIX = "guest-"
        const val KEEP = 5
    }

    private fun stamp(ms: Long) =
        SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(ms))

    fun logs(): List<File> =
        dir.listFiles { f -> f.isFile && f.name.startsWith(prefix) && f.name.endsWith(".log") }
            ?.sortedBy { it.name }.orEmpty()

    fun newest(): File? = logs().lastOrNull()

    /** Creates a fresh (empty) log file and prunes old ones so at most [keep] remain. */
    fun newLog(nowMs: Long): File {
        dir.mkdirs()
        val base = prefix + stamp(nowMs)
        var f = File(dir, "$base.log")
        var i = 1
        while (f.exists()) f = File(dir, "$base-${i++}.log")
        f.createNewFile()
        prune()
        return f
    }

    fun prune() {
        val all = logs()
        if (all.size > keep) all.take(all.size - keep).forEach { it.delete() }
    }
}
