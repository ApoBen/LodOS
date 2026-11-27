package org.lodos.app.bugreport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.util.zip.ZipFile

class BugReportTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val info = linkedMapOf("manufacturer" to "Xiaomi", "model" to "POCO X7 Pro", "sdk" to "35")

    private fun input(logcat: String = "I lodos: hello\n", files: List<File> = emptyList()) =
        BugReport.Input(info, "SetupState(step=ROOTFS)", logcat, files, 1_700_000_000_000L)

    private fun entries(zip: File): Map<String, ByteArray> = ZipFile(zip).use { zf ->
        zf.entries().toList().associate { e -> e.name to zf.getInputStream(e).use { it.readBytes() } }
    }

    @Test
    fun zipContainsReportAndLogs() {
        val logs = tmp.newFolder("logs")
        File(logs, "session.log").writeText("wayland started\n")
        File(logs, "ignore.txt").writeText("not a log\n")
        val zip = BugReport.writeZip(input(files = logs.listFiles()!!.toList()), tmp.newFolder("out"))
        assertTrue(zip.name.startsWith("bugreport-") && zip.name.endsWith(".zip"))
        assertEquals("bugreport-20231114-221320.zip", zip.name)
        val e = entries(zip)
        assertEquals(setOf("report.md", "logs/logcat.txt", "logs/session.log"), e.keys)
        val report = String(e.getValue("report.md"), StandardCharsets.UTF_8)
        assertTrue(report.contains("POCO X7 Pro"))
        assertTrue(report.contains("SetupState(step=ROOTFS)"))
        assertTrue(report.contains("logs/session.log"))
        assertTrue(report.contains("I lodos: hello"))
        assertEquals("wayland started\n", String(e.getValue("logs/session.log"), StandardCharsets.UTF_8))
        assertEquals(report, BugReport.readReport(zip))
    }

    @Test
    fun logFileCappedToTail() {
        val logs = tmp.newFolder("logs")
        val big = File(logs, "big.log")
        val sb = StringBuilder()
        var i = 0
        while (sb.length < 2 * BugReport.LOG_FILE_CAP_BYTES) sb.append("line ").append(i++).append('\n')
        big.writeText(sb.toString())
        val zip = BugReport.writeZip(input(files = listOf(big)), tmp.newFolder("out"))
        val data = String(entries(zip).getValue("logs/big.log"), StandardCharsets.UTF_8)
        assertTrue(data.toByteArray().size <= BugReport.LOG_FILE_CAP_BYTES)
        assertTrue(data.endsWith("line ${i - 1}\n"))
        assertTrue("tail starts at a line boundary", data.startsWith("line "))
        assertFalse(data.contains("line 0\n"))
    }

    @Test
    fun logcatCapped() {
        val lc = "x".repeat(99) + "\n"
        val zip = BugReport.writeZip(input(logcat = lc.repeat(30_000)), tmp.newFolder("out"))
        assertTrue(entries(zip).getValue("logs/logcat.txt").size <= BugReport.LOGCAT_CAP_BYTES)
    }

    @Test
    fun tailHelpers() {
        val d = "aaa\nbbb\nccc\n".toByteArray()
        assertEquals("ccc\n", String(BugReport.tail(d, 5)))
        assertTrue(BugReport.tail(d, 100).contentEquals(d))
        assertEquals("é", BugReport.truncateUtf8("éé", 3))
    }

    @Test
    fun redactRemovesEmailTokensAndHomePaths() {
        val raw = """
            user mail: john.doe+x@example.com
            GITHUB=ghp_abcdefghijklmnopqrstuvwxyz0123456789
            pat github_pat_11ABCDEFG0123456789_abcdefghijklmnop
            Authorization: Bearer abc.def-ghi_jkl12345
            api_key=supersecretvalue123 password: hunter22
            jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N
            path /home/alice/projects/lodos and /Users/bob/x and C:\Users\carol\y
            keep /data/data/org.lodos.app/files/rootfs
        """.trimIndent()
        val r = BugReport.redact(raw)
        for (secret in listOf("john.doe", "example.com", "ghp_", "github_pat_", "abc.def-ghi_jkl12345",
                "supersecretvalue123", "hunter22", "eyJhbGci", "alice", "bob", "carol")) {
            assertFalse("leaked $secret in:\n$r", r.contains(secret))
        }
        assertTrue(r.contains("<email>"))
        assertTrue(r.contains("<token>"))
        assertTrue(r.contains("/home/<user>/projects/lodos"))
        assertTrue(r.contains("/data/data/org.lodos.app/files/rootfs"))
    }

    @Test
    fun zipIsRedacted() {
        val logs = tmp.newFolder("logs")
        File(logs, "a.log").writeText("contact me@mail.org token=ghp_abcdefghijklmnopqrstuvwxyz0123456789\n")
        val zip = BugReport.writeZip(
            input(logcat = "E app: /home/someone/file\n", files = listOf(File(logs, "a.log"))),
            tmp.newFolder("out"))
        val all = entries(zip).values.joinToString("\n") { String(it, StandardCharsets.UTF_8) }
        assertFalse(all.contains("me@mail.org"))
        assertFalse(all.contains("ghp_"))
        assertFalse(all.contains("someone"))
    }

    @Test
    fun issueBodyCappedAndUrlEncoded() {
        val longReport = "## r\n\n```\n" + "log line with ünïcode\n".repeat(2000)
        val body = BugReport.issueBody(longReport)
        assertTrue(body.toByteArray(StandardCharsets.UTF_8).size <= BugReport.ISSUE_BODY_MAX_BYTES)
        assertTrue(body.contains("truncated"))
        assertEquals("fences balanced", 0, Regex("(?m)^```").findAll(body).count() % 2)

        val short = "## r\nmail a@b.co\n"
        assertEquals("## r\nmail <email>\n", BugReport.issueBody(short))

        val url = BugReport.issueUrl("Bug: x & y", body, "https://github.com/o/r")
        assertTrue(url.startsWith("https://github.com/o/r/issues/new?title=Bug%3A%20x%20%26%20y&body="))
        assertFalse(url.contains(" "))
        val decoded = URLDecoder.decode(url.substringAfter("&body="), "UTF-8")
        assertEquals(body, decoded)
    }
}
