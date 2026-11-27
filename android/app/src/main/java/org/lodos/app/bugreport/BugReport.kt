package org.lodos.app.bugreport

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.net.URLEncoder
import java.nio.charset.StandardCharsets
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Pure-JVM core of the in-app bug reporter (T040). No android.* imports: everything
 * Android-specific is gathered by [BugReportCollector] and passed in as plain values.
 */
object BugReport {
    const val ISSUE_REPO_URL = "https://github.com/OWNER/LodOS" // placeholder, set before release
    const val LOG_FILE_CAP_BYTES = 512 * 1024
    const val LOGCAT_CAP_BYTES = 1024 * 1024
    const val ISSUE_BODY_MAX_BYTES = 6000
    const val REPORT_NAME = "report.md"
    const val ZIP_PREFIX = "bugreport-"
    /** T048 guest session logs: only the newest guest-*.log, last N lines. */
    const val GUEST_LOG_PREFIX = "guest-"
    const val GUEST_LOG_TAIL_LINES = 2000
    private const val TRUNC_NOTE = "\n\n…(truncated; full report + logs in the attached bugreport zip)\n"

    data class Input(
        val deviceInfo: Map<String, String>,
        val setupState: String,
        val logcat: String,
        /** name -> file; only *.log files are expected. */
        val logFiles: List<File>,
        val timestampMs: Long
    )

    // ---- redaction ----
    private val REDACTIONS: List<Pair<Regex, String>> = listOf(
        // GitHub tokens
        Regex("""\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}\b""") to "<token>",
        Regex("""\bgithub_pat_[A-Za-z0-9_]{20,}\b""") to "<token>",
        // AWS access key id, Google API key, Slack token
        Regex("""\bAKIA[0-9A-Z]{16}\b""") to "<token>",
        Regex("""\bAIza[0-9A-Za-z_\-]{35}\b""") to "<token>",
        Regex("""\bxox[abposr]-[A-Za-z0-9\-]{10,}\b""") to "<token>",
        // JWT
        Regex("""\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}""") to "<token>",
        // Authorization: Bearer xxx / Basic xxx
        Regex("""(?i)\b(bearer|basic)\s+[A-Za-z0-9._~+/=\-]{8,}""") to "$1 <token>",
        // key=value / key: value secrets
        Regex("""(?i)\b([A-Za-z0-9_\-]*(?:token|passwd|password|secret|api[_\-]?key|auth)[A-Za-z0-9_\-]*)(\s*[=:]\s*)("[^"]*"|'[^']*'|[^\s,;&]+)""") to "$1$2<redacted>",
        // e-mail addresses
        Regex("""[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}""") to "<email>",
        // home directories
        Regex("""/home/[^/\s:'"]+""") to "/home/<user>",
        Regex("""/Users/[^/\s:'"]+""") to "/Users/<user>",
        Regex("""(?i)\b([A-Z]:\\Users\\)[^\\\s:'"]+""") to "$1<user>"
    )

    fun redact(text: String): String =
        REDACTIONS.fold(text) { acc, (re, rep) -> re.replace(acc, rep) }

    // ---- size caps ----
    /** Last [cap] bytes of [data], realigned to the next line start when truncated. */
    fun tail(data: ByteArray, cap: Int): ByteArray {
        if (data.size <= cap) return data
        var start = data.size - cap
        val nl = (start until data.size).firstOrNull { data[it] == '\n'.code.toByte() }
        if (nl != null && nl + 1 < data.size) start = nl + 1
        return data.copyOfRange(start, data.size)
    }

    /** Reads at most the last [cap] bytes of [file] without loading the whole file. */
    fun tailFile(file: File, cap: Int): ByteArray {
        RandomAccessFile(file, "r").use { raf ->
            val len = raf.length()
            val n = minOf(len, cap.toLong() + 1).toInt() // +1 to detect truncation for tail()
            val buf = ByteArray(n)
            raf.seek(len - n)
            raf.readFully(buf)
            return if (len > cap) tail(buf, cap) else buf
        }
    }

    /** Truncates [s] to at most [maxBytes] UTF-8 bytes on a code point boundary. */
    fun truncateUtf8(s: String, maxBytes: Int): String {
        val bytes = s.toByteArray(StandardCharsets.UTF_8)
        if (bytes.size <= maxBytes) return s
        var end = maxBytes
        while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) end--
        return String(bytes, 0, end, StandardCharsets.UTF_8)
    }

    /** Last [n] lines of [text]. */
    fun tailLines(text: String, n: Int): String {
        val lines = text.split('\n')
        val trailingNl = text.endsWith("\n")
        val body = if (trailingNl) lines.dropLast(1) else lines
        if (body.size <= n) return text
        return body.takeLast(n).joinToString("\n") + if (trailingNl) "\n" else ""
    }

    fun isGuestLog(f: File) = f.name.startsWith(GUEST_LOG_PREFIX) && f.name.endsWith(".log")

    /** All non-guest *.log files + only the newest guest-*.log (names sort by timestamp). */
    fun selectLogs(files: List<File>): List<File> {
        val logs = files.filter { it.isFile && it.name.endsWith(".log") }
        val newestGuest = logs.filter { isGuestLog(it) }.maxByOrNull { it.name }
        return logs.filter { !isGuestLog(it) } + listOfNotNull(newestGuest)
    }

    // ---- report ----
    fun timestamp(ms: Long): String =
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }
            .format(Date(ms))

    fun buildReportMd(input: Input, logNames: List<String>): String {
        val sb = StringBuilder()
        sb.append("## LodOS bug report\n\n")
        sb.append("**Describe the problem:** <!-- what happened, what you expected, steps to reproduce -->\n\n")
        sb.append("### Device\n\n")
        sb.append("| key | value |\n|---|---|\n")
        for ((k, v) in input.deviceInfo) sb.append("| ").append(k).append(" | ").append(v.replace("|", "/")).append(" |\n")
        sb.append("| report time (UTC) | ").append(timestamp(input.timestampMs)).append(" |\n")
        sb.append("\n### Setup state\n\n```\n").append(input.setupState.trim()).append("\n```\n")
        sb.append("\n### Attached logs\n\n")
        if (logNames.isEmpty()) sb.append("(none)\n") else logNames.forEach { sb.append("- logs/").append(it).append('\n') }
        val lastLines = input.logcat.trimEnd().lines().takeLast(40).joinToString("\n")
        if (lastLines.isNotBlank()) {
            sb.append("\n### Last logcat lines\n\n```\n").append(lastLines).append("\n```\n")
        }
        return redact(sb.toString())
    }

    /** Writes `<outDir>/bugreport-<ts>.zip` with report.md + logs/. Returns the zip. */
    fun writeZip(input: Input, outDir: File): File {
        outDir.mkdirs()
        val logs = LinkedHashMap<String, ByteArray>()
        val logcatBytes = tail(input.logcat.toByteArray(StandardCharsets.UTF_8), LOGCAT_CAP_BYTES)
        logs["logcat.txt"] = redact(String(logcatBytes, StandardCharsets.UTF_8)).toByteArray(StandardCharsets.UTF_8)
        for (f in selectLogs(input.logFiles).sortedBy { it.name }) {
            var text = String(tailFile(f, LOG_FILE_CAP_BYTES), StandardCharsets.UTF_8)
            if (isGuestLog(f)) text = tailLines(text, GUEST_LOG_TAIL_LINES)
            val red = redact(text).toByteArray(StandardCharsets.UTF_8)
            // redaction placeholders can grow content slightly; re-apply the cap
            logs[f.name] = tail(red, LOG_FILE_CAP_BYTES)
        }
        val report = buildReportMd(input, logs.keys.toList())
        val zip = File(outDir, ZIP_PREFIX + timestamp(input.timestampMs) + ".zip")
        ZipOutputStream(FileOutputStream(zip)).use { zos ->
            zos.putNextEntry(ZipEntry(REPORT_NAME))
            zos.write(report.toByteArray(StandardCharsets.UTF_8))
            zos.closeEntry()
            for ((name, bytes) in logs) {
                zos.putNextEntry(ZipEntry("logs/$name"))
                zos.write(bytes)
                zos.closeEntry()
            }
        }
        return zip
    }

    /** Reads report.md back out of a zip produced by [writeZip]. */
    fun readReport(zip: File): String {
        java.util.zip.ZipFile(zip).use { zf ->
            val e = zf.getEntry(REPORT_NAME) ?: return ""
            zf.getInputStream(e).use { ins ->
                val out = ByteArrayOutputStream()
                ins.copyTo(out)
                return String(out.toByteArray(), StandardCharsets.UTF_8)
            }
        }
    }

    // ---- GitHub issue ----
    fun issueBody(reportMd: String, maxBytes: Int = ISSUE_BODY_MAX_BYTES): String {
        val red = redact(reportMd)
        if (red.toByteArray(StandardCharsets.UTF_8).size <= maxBytes) return red
        val noteLen = TRUNC_NOTE.toByteArray(StandardCharsets.UTF_8).size
        val cut = truncateUtf8(red, maxBytes - noteLen - 4)
        // keep markdown sane: close an unterminated code fence (4 bytes reserved above)
        val fences = Regex("(?m)^```").findAll(cut).count()
        val closer = if (fences % 2 == 1) "\n```" else ""
        return cut + closer + TRUNC_NOTE
    }

    fun issueUrl(title: String, body: String, repoUrl: String = ISSUE_REPO_URL): String {
        fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")
        return "$repoUrl/issues/new?title=${enc(title)}&body=${enc(body)}"
    }

    fun defaultTitle(deviceInfo: Map<String, String>): String =
        "Bug: <short summary> (" + (deviceInfo["model"] ?: "device") + ", SDK " + (deviceInfo["sdk"] ?: "?") + ")"
}
