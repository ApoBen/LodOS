package org.lodos.app.setup

import com.sun.net.httpserver.HttpServer
import org.junit.Assert.*
import org.junit.Test
import java.net.InetSocketAddress
import java.security.MessageDigest
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class HttpRootfsRepoTest {
    private fun server(bytes: ByteArray, slow: Boolean = false): HttpServer {
        val s = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        s.createContext("/rootfs") { exchange ->
            val range = exchange.requestHeaders.getFirst("Range")?.substringAfter("bytes=")?.substringBefore('-')?.toIntOrNull() ?: 0
            val body = bytes.copyOfRange(range.coerceAtMost(bytes.size), bytes.size)
            if (range > 0) exchange.responseHeaders.add("Content-Range", "bytes $range-${bytes.lastIndex}/${bytes.size}")
            exchange.sendResponseHeaders(if (range > 0) 206 else 200, if (slow) 0 else body.size.toLong())
            if (slow) {
                body.forEach { b -> try { exchange.responseBody.write(b.toInt()); exchange.responseBody.flush(); Thread.sleep(2) } catch (_: Exception) { } }
            } else exchange.responseBody.use { it.write(body) }
        }
        s.start(); return s
    }
    private fun repo(dir: File, spec: RootfsSpec = RootfsSpec("", "", "test"), extract: RootfsExtractor = RootfsExtractor { _, _ -> }) =
        HttpRootfsRepo(File(dir, "cache"), File(dir, "rootfs"), spec, extract, 1000, 1000)
    private fun digest(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    @Test fun downloadsAndReportsProgress() {
        val data = ByteArray(2048) { it.toByte() }; val s = server(data); val d = Files.createTempDirectory("rfs").toFile()
        try { val progress = mutableListOf<Int>(); val path = repo(d).download("http://127.0.0.1:${s.address.port}/rootfs") { progress += it }.getOrThrow()
            assertArrayEquals(data, File(path).readBytes()); assertTrue(progress.isNotEmpty())
        } finally { s.stop(0); d.deleteRecursively() }
    }

    @Test fun resumesPartialArchiveWithRange() {
        val data = ByteArray(3072) { (it % 127).toByte() }; val d = Files.createTempDirectory("rfs").toFile(); File(d, "cache").mkdirs()
        File(d, "cache/rootfs.part").writeBytes(data.copyOfRange(0, 777)); val s = server(data)
        try { val path = repo(d).download("http://127.0.0.1:${s.address.port}/rootfs").getOrThrow(); assertArrayEquals(data, File(path).readBytes()) }
        finally { s.stop(0); d.deleteRecursively() }
    }

    @Test fun rejectsBadDigest() {
        val f = Files.createTempFile("archive", ".tgz").toFile(); f.writeText("bad")
        try { assertTrue(repo(f.parentFile).verifySha256(f.path, "0".repeat(64)).isFailure) } finally { f.delete() }
    }

    @Test fun verifiesGoodDigest() {
        val b = "contents".toByteArray(); val f = Files.createTempFile("archive", ".tgz").toFile(); f.writeBytes(b)
        try { assertTrue(repo(f.parentFile).verifySha256(f.path, digest(b)).isSuccess) } finally { f.delete() }
    }

    @Test fun parsesArchitectureManifest() {
        val spec = HttpRootfsRepo.parseManifest("""{"arm64":{"url":"https://a","sha256":"${"a".repeat(64)}"},"x86_64":{"url":"https://x","sha256":"${"b".repeat(64)}","version":"v2"}}""", "x86_64")
        assertEquals("https://x", spec.url); assertEquals("v2", spec.version)
    }

    @Test fun cancellationStopsDownload() {
        val data = ByteArray(100_000) { 42 }; val s = server(data, slow = true); val d = Files.createTempDirectory("rfs").toFile(); val r = repo(d)
        val done = CountDownLatch(1); var result: Result<String>? = null
        val t = Thread { result = r.download("http://127.0.0.1:${s.address.port}/rootfs") {}; done.countDown() }; t.start()
        Thread.sleep(40); r.cancel(); assertTrue(done.await(3, TimeUnit.SECONDS)); assertTrue(result!!.isFailure)
        s.stop(0); d.deleteRecursively()
    }
}
