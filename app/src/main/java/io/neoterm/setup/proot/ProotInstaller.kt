package io.neoterm.setup.proot

import android.app.ProgressDialog
import android.system.Os
import androidx.appcompat.app.AppCompatActivity
import io.neoterm.backend.EmulatorDebug
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.ResultListener
import io.neoterm.utils.NLog
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * A proot mód telepítője: letölti a proot binárist és a kiválasztott disztró
 * rootfs-ét, majd kibontja a `.tar.xz`-t egy valódi FHS-fába.
 *
 * A Termux-stílusú [io.neoterm.setup.SetupThread]-del ellentétben itt nincs
 * SYMLINKS.txt és nincs egyedi PREFIX: a tar tartalmazza a tényleges
 * könyvtárfát, symlinkeket, jogosultságokat — pont ahogy egy normál Linux
 * gyökér kinéz.
 *
 * @author kiva
 */
class ProotInstaller(
  private val activity: AppCompatActivity,
  private val distro: Distro,
  private val arch: String,
  private val baseUrl: String,
  private val resultListener: ResultListener,
  private val progressDialog: ProgressDialog
) : Thread() {

  override fun run() {
    try {
      installProotBinary()
      installRootfs()
      finalizeFilesystem()
      postSuccess()
    } catch (e: Exception) {
      NLog.e(EmulatorDebug.LOG_TAG, "Proot bootstrap error: ${e.localizedMessage}")
      postFailure(e)
    } finally {
      dismissProgress()
    }
  }

  // ── proot bináris ─────────────────────────────────────────────────────
  private fun installProotBinary() {
    // Ha a proot bele van csomagolva az APK-ba (libproot.so), nincs mit
    // letölteni — csak a guest tmp-könyvtárat biztosítjuk.
    if (ProotManager.bundledProotPath() != null) {
      File(NeoTermPath.PROOT_TMP_PATH).mkdirs()
      return
    }

    setMessage("Downloading proot…")
    val target = File(NeoTermPath.PROOT_BIN_PATH)
    target.parentFile?.mkdirs()
    File(NeoTermPath.PROOT_TMP_PATH).mkdirs()

    val url = Distro.prootUrl(baseUrl, arch)
    openStream(url).use { input ->
      FileOutputStream(target).use { output -> input.copyTo(output, 64 * 1024) }
    }
    // 0700 — csak az app UID futtathatja.
    Os.chmod(target.absolutePath, 448 /* 0700 */)
  }

  // ── rootfs ────────────────────────────────────────────────────────────
  private fun installRootfs() {
    setMessage("Downloading ${distro.displayName} rootfs…")

    val stagingDir = File("${distro.rootfsPath()}-staging")
    if (stagingDir.exists()) deleteRecursively(stagingDir)
    if (!stagingDir.mkdirs()) {
      throw RuntimeException("Unable to create staging dir: ${stagingDir.absolutePath}")
    }

    val expectedSha = fetchSha256(distro.rootfsSha256Url(baseUrl, arch))
    val digest = MessageDigest.getInstance("SHA-256")

    val url = distro.rootfsUrl(baseUrl, arch)
    setMessage("Extracting ${distro.displayName} rootfs…")
    openStream(url).use { rawInput ->
      val counting = DigestStream(rawInput, digest)
      TarArchiveInputStream(XZInputStream(BufferedInputStream(counting))).use { tar ->
        extractTar(tar, stagingDir)
      }
    }

    if (expectedSha != null) {
      val actual = digest.digest().joinToString("") { "%02x".format(it) }
      if (!actual.equals(expectedSha, ignoreCase = true)) {
        deleteRecursively(stagingDir)
        throw RuntimeException("Rootfs checksum mismatch (expected $expectedSha, got $actual)")
      }
    }

    val finalDir = File(distro.rootfsPath())
    if (finalDir.exists()) deleteRecursively(finalDir)
    if (!stagingDir.renameTo(finalDir)) {
      throw RuntimeException("Unable to move staging rootfs into place")
    }
  }

  private fun extractTar(tar: TarArchiveInputStream, destRoot: File) {
    val buffer = ByteArray(64 * 1024)
    val deferredSymlinks = ArrayList<Pair<String, File>>()
    var entry: TarArchiveEntry? = tar.nextTarEntry
    val destPath = destRoot.canonicalPath

    while (entry != null) {
      val outFile = File(destRoot, entry.name)
      // Path-traversal védelem (zip-slip a tar-ban is lehetséges).
      if (!outFile.canonicalPath.startsWith(destPath + File.separator) &&
        outFile.canonicalPath != destPath
      ) {
        throw RuntimeException("Refusing path-traversal entry: ${entry.name}")
      }

      when {
        entry.isDirectory -> outFile.mkdirs()

        entry.isSymbolicLink -> {
          // A symlinkeket a végén hozzuk létre, mert a célja még nem biztos,
          // hogy kibontásra került.
          deferredSymlinks.add(entry.linkName to outFile)
        }

        entry.isLink -> {
          // Hardlink: a tar-ban a célja már korábban kibontásra került.
          outFile.parentFile?.mkdirs()
          val linkTarget = File(destRoot, entry.linkName)
          if (outFile.exists()) outFile.delete()
          try {
            Os.link(linkTarget.absolutePath, outFile.absolutePath)
          } catch (e: Exception) {
            // Ha a hardlink nem megy (pl. fs nem támogatja), másoljuk.
            linkTarget.copyTo(outFile, overwrite = true)
          }
        }

        entry.isFile -> {
          outFile.parentFile?.mkdirs()
          FileOutputStream(outFile).use { out ->
            var read = tar.read(buffer)
            while (read != -1) {
              out.write(buffer, 0, read)
              read = tar.read(buffer)
            }
          }
          chmodSafe(outFile, entry.mode)
        }

        // Device/char/fifo node-ok: nem tudunk mknod-ot root nélkül, és
        // nincs is rá szükség — a /dev-et proot bind-mounttal adjuk.
        else -> { /* skip */ }
      }
      entry = tar.nextTarEntry
    }

    for ((linkName, linkFile) in deferredSymlinks) {
      linkFile.parentFile?.mkdirs()
      if (linkFile.exists()) linkFile.delete()
      try {
        Os.symlink(linkName, linkFile.absolutePath)
      } catch (e: Exception) {
        NLog.e("ProotInstaller", "symlink failed ${linkFile.name} -> $linkName")
      }
    }
  }

  // ── befejezés ─────────────────────────────────────────────────────────
  private fun finalizeFilesystem() {
    val rootfs = File(distro.rootfsPath())
    // A guest minimális írható könyvtárai.
    File(rootfs, "tmp").mkdirs()
    File(rootfs, "root").mkdirs()
    File(rootfs, "dev").mkdirs()
    File(rootfs, "proc").mkdirs()
    File(rootfs, "sys").mkdirs()
    // resolv.conf, hogy a guestben legyen DNS.
    runCatching {
      File(rootfs, "etc").mkdirs()
      File(rootfs, "etc/resolv.conf").writeText("nameserver 8.8.8.8\nnameserver 8.8.4.4\n")
      File(rootfs, "etc/hosts").writeText("127.0.0.1 localhost\n")
    }
  }

  // ── hálózat / IO segédek ──────────────────────────────────────────────
  private fun openStream(urlString: String): java.io.InputStream {
    val connection = URL(urlString).openConnection() as HttpURLConnection
    connection.connectTimeout = 15000
    connection.readTimeout = 30000
    connection.instanceFollowRedirects = true
    if (connection.responseCode !in 200..299) {
      throw RuntimeException("HTTP ${connection.responseCode} for $urlString")
    }
    return BufferedInputStream(connection.inputStream)
  }

  private fun fetchSha256(urlString: String): String? {
    return try {
      openStream(urlString).bufferedReader().use { reader ->
        // formátum: "<hex>  <filename>"
        reader.readLine()?.trim()?.split(Regex("\\s+"))?.firstOrNull()
      }
    } catch (e: Exception) {
      NLog.e("ProotInstaller", "No sha256 available ($urlString), skipping verification")
      null
    }
  }

  private fun chmodSafe(file: File, mode: Int) {
    try {
      // a tar mode alsó 12 bitje a jogosultság (sticky/setuid/setgid + rwx)
      Os.chmod(file.absolutePath, mode and 4095 /* 07777 */)
    } catch (e: Exception) {
      // best-effort
    }
  }

  private fun deleteRecursively(file: File) {
    if (file.isDirectory) file.listFiles()?.forEach { deleteRecursively(it) }
    file.delete()
  }

  // ── UI thread visszahívások ───────────────────────────────────────────
  private fun setMessage(message: String) = activity.runOnUiThread {
    runCatching { progressDialog.setMessage(message) }
  }

  private fun postSuccess() = activity.runOnUiThread {
    runCatching { resultListener.onResult(null) }
  }

  private fun postFailure(e: Exception) = activity.runOnUiThread {
    runCatching { resultListener.onResult(e) }
  }

  private fun dismissProgress() = activity.runOnUiThread {
    runCatching { progressDialog.dismiss() }
  }

  /** Áteresztő stream, amely menet közben sha256-ot számol a nyers bájtokra. */
  private class DigestStream(
    private val wrapped: java.io.InputStream,
    private val digest: MessageDigest
  ) : java.io.InputStream() {
    override fun read(): Int {
      val b = wrapped.read()
      if (b != -1) digest.update(b.toByte())
      return b
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
      val read = wrapped.read(b, off, len)
      if (read > 0) digest.update(b, off, read)
      return read
    }

    override fun close() = wrapped.close()
  }
}
