package io.neoterm.setup.proot

import android.os.SystemClock
import io.neoterm.App
import io.neoterm.component.config.NeoTermPath
import java.io.File
import java.util.Locale

/**
 * A guest `/dev/kmsg` puffer **host-oldali** írója. A NeoTerm a saját
 * eszköz-/bridge-eseményeit (USB plug/unplug stb.) ide fűzi, így a guest
 * `dmesg`-ében megjelennek — ezek a „prooton belüli", kernel-szerű események,
 * amiket az Android valódi kernel-logja (tiltott) nem ad át a guestnek.
 *
 * Ugyanazt a backing fájlt írja, amit a [ProotManager] a `/dev/kmsg`-re köt
 * (`<sysdata>/kmsg`). Append-módú írás (a guest is O_APPEND-del ír — lásd a
 * proot-patchet), így a sorok nem keverednek; a méret-sapkát a launch intézi.
 *
 * Minden sor elé a NeoTerm-futásidőből vett időbélyeget tesz `"<sec>;<msg>"`
 * formában (a valódi kernel kmsg-hez hasonlóan a beíráskor rögzítve), így a
 * `dmesg`-shim soronként a TÉNYLEGES naplózási időt mutatja — nem egy közös,
 * olvasáskori értéket.
 */
object Kmsg {
  private val file by lazy { File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata/kmsg") }

  /** Egy sort fűz a kmsg pufferhez időbélyeggel (a záró újsort normalizálja). Hibatűrő. */
  @Synchronized
  fun log(msg: String) {
    runCatching {
      val f = file
      f.parentFile?.let { if (!it.isDirectory) it.mkdirs() }
      f.appendText(stamp() + ";" + msg.trimEnd('\n', '\r') + "\n")
    }
  }

  /** NeoTerm-futásidő másodpercben (az app-folyamat indulása óta), kernel-uptime
   *  stílusban — ugyanaz az óra, mint a `/proc/uptime` fake-é. */
  private fun stamp(): String {
    val now = SystemClock.elapsedRealtime()
    val start = App.startElapsedRealtimeMs
    val upMs = if (start in 1L..now) now - start else now
    return String.format(Locale.ROOT, "%.6f", upMs / 1000.0)
  }
}
