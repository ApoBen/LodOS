package org.lodos.app.setup

/**
 * Metadata describing a rootfs archive to fetch and verify.
 */
data class RootfsSpec(
    val url: String,
    val sha256: String,
    val version: String = "debian-trixie-arm64"
)

/**
 * Abstraction over rootfs download, SHA-256 verification, and extraction.
 * Implementations provide download, checksum verification, and extraction.
 */
interface RootfsRepo {
    fun isInstalled(): Boolean
    fun download(url: String, onProgress: (Int) -> Unit): Result<String>
    fun verifySha256(archivePath: String, expectedSha256: String): Result<Unit>
    fun extract(archivePath: String, onProgress: (Int) -> Unit): Result<Unit>
    fun cancel() {}
}
