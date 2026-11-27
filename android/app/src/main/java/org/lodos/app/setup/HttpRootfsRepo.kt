package org.lodos.app.setup

import android.content.Context
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Locale

fun interface RootfsExtractor { fun extract(archive: File, destination: File) }

/** Resumable HTTP rootfs downloader with digest verification and native extraction. */
class HttpRootfsRepo(
    private val cacheDir: File,
    private val rootfsDir: File,
    private val spec: RootfsSpec,
    private val extractor: RootfsExtractor,
    private val connectTimeoutMs: Int = 15_000,
    private val readTimeoutMs: Int = 30_000
) : RootfsRepo {
    @Volatile private var cancelled = false
    @Volatile private var connection: HttpURLConnection? = null

    constructor(context: Context) : this(
        File(context.cacheDir, "rootfs"), File(context.filesDir, "rootfs"),
        rootfsSpec(context), RootfsExtractor { archive, dest ->
            System.loadLibrary("lodos_rootfs")
            NativeRootfs.extract(archive.absolutePath, dest.absolutePath)
        }
    )

    override fun isInstalled() = File(rootfsDir, "etc/debian_version").isFile

    override fun download(url: String, onProgress: (Int) -> Unit): Result<String> = runCatching {
        cancelled = false
        cacheDir.mkdirs()
        val part = File(cacheDir, "rootfs.part")
        val target = File(cacheDir, "rootfs.archive")
        var offset = part.length()
        if (!part.exists()) part.createNewFile()
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = connectTimeoutMs; readTimeout = readTimeoutMs
            if (offset > 0) setRequestProperty("Range", "bytes=$offset-")
        }
        connection = conn
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw java.io.IOException("HTTP $code")
            if (offset > 0 && code != HttpURLConnection.HTTP_PARTIAL) {
                offset = 0
                FileOutputStream(part, false).close()
            }
            val total = conn.getHeaderField("Content-Range")?.substringAfterLast('/')?.toLongOrNull()
                ?: conn.getHeaderField("Content-Length")?.toLongOrNull()?.plus(offset)
                ?: -1L
            conn.inputStream.use { input -> FileOutputStream(part, offset > 0).use { output ->
                val buf = ByteArray(64 * 1024)
                var count = offset
                while (true) {
                    if (cancelled || Thread.currentThread().isInterrupted) throw java.io.IOException("Download cancelled")
                    val n = input.read(buf); if (n < 0) break
                    output.write(buf, 0, n); count += n
                    if (total > 0) onProgress(((count * 100 / total).coerceIn(0, 100)).toInt())
                }
            } }
            if (cancelled) throw java.io.IOException("Download cancelled")
            if (target.exists()) target.delete()
            if (!part.renameTo(target)) throw java.io.IOException("Cannot finalize rootfs archive")
            onProgress(100)
            target.absolutePath
        } finally { conn.disconnect(); connection = null }
    }

    override fun verifySha256(archivePath: String, expectedSha256: String): Result<Unit> = runCatching {
        require(expectedSha256.matches(Regex("(?i)[0-9a-f]{64}"))) { "Invalid SHA-256 in rootfs manifest" }
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(archivePath).use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) { val n = input.read(buf); if (n < 0) break; digest.update(buf, 0, n) }
        }
        val actual = digest.digest().joinToString("") { "%02x".format(Locale.ROOT, it) }
        check(actual.equals(expectedSha256, ignoreCase = true)) { "SHA-256 mismatch" }
    }

    override fun extract(archivePath: String, onProgress: (Int) -> Unit): Result<Unit> = runCatching {
        onProgress(0)
        extractor.extract(File(archivePath), rootfsDir)
        check(isInstalled()) { "Extracted rootfs is missing etc/debian_version" }
        onProgress(100)
    }

    override fun cancel() { cancelled = true; connection?.disconnect() }

    companion object {
        fun parseManifest(json: String, arch: String): RootfsSpec {
            val block = Regex("\\\"${Regex.escape(arch)}\\\"\\s*:\\s*\\{([^}]*)}").find(json)?.groupValues?.get(1)
                ?: error("Missing $arch in rootfs manifest")
            fun field(name: String) = Regex("\\\"${Regex.escape(name)}\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"")
                .find(block)?.groupValues?.get(1)
            return RootfsSpec(field("url") ?: error("Missing $arch URL"), field("sha256") ?: error("Missing $arch SHA-256"), field("version") ?: "debian-trixie-$arch")
        }
        fun rootfsSpec(context: Context): RootfsSpec {
            val json = context.assets.open("rootfs-manifest.json").bufferedReader().use { it.readText() }
            val arch = if (android.os.Build.SUPPORTED_ABIS.any { it.contains("arm64") }) "arm64" else "x86_64"
            return parseManifest(json, arch)
        }
    }
}

internal object NativeRootfs {
    @JvmStatic external fun extract(src: String, dst: String): String
}
