package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath
import java.io.File

/**
 * A guest `/dev/kmsg` puffer **host-oldali** írója. A NeoTerm a saját
 * eszköz-/bridge-eseményeit (USB plug/unplug stb.) ide fűzi, így a guest
 * `dmesg`-ében megjelennek — ezek a „prooton belüli", kernel-szerű események,
 * amiket az Android valódi kernel-logja (tiltott) nem ad át a guestnek.
 *
 * Ugyanazt a backing fájlt írja, amit a [ProotManager] a `/dev/kmsg`-re köt
 * (`<sysdata>/kmsg`). Append-módú írás (a guest is O_APPEND-del ír — lásd a
 * proot-patchet), így a sorok nem keverednek; a méret-sapkát a launch intézi.
 */
object Kmsg {
  private val file by lazy { File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata/kmsg") }

  /** Egy sort fűz a kmsg pufferhez (a záró újsort normalizálja). Hibatűrő. */
  @Synchronized
  fun log(msg: String) {
    runCatching {
      val f = file
      f.parentFile?.let { if (!it.isDirectory) it.mkdirs() }
      f.appendText(msg.trimEnd('\n', '\r') + "\n")
    }
  }
}
