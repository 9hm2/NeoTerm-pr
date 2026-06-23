package io.neoterm.setup.proot

import android.os.SystemClock
import io.neoterm.component.config.NeoTermPath
import java.io.File

/**
 * Fake `/proc` (és néhány `/proc/sys`) tartalom a proot guest számára — a
 * termux/proot-distro `sysdata.py` mintájára.
 *
 * Az Android `/proc`-ja korlátozott (SELinux + hidepid), több fájl
 * olvashatatlan vagy hiányos, ezért az olyan eszközök, amik ezeket olvassák
 * (`ps`, `top`, `htop`, `uptime`, `free`, `vmstat`), hibára futnak — pl.
 * „Unable to get system boot time” (a `/proc/stat` `btime` sora miatt).
 *
 * Megoldás: valószerű tartalmú fake fájlokat írunk egy host-oldali
 * `sysdata/` könyvtárba, és proot `-b fake:/proc/...` bind-mounttal a guestbe
 * tesszük — DE csak azokra a célokra, amik a valóságban olvashatatlanok
 * (a stat esetén akkor is, ha hiányzik a `btime`).
 *
 * A SELinux miatt a valódi `/proc` többnyire zárt, de néhány érték kinyerhető
 * Android API-ból `/proc` olvasása nélkül is: az `uptime` a tényleges
 * eszköz-uptime ([SystemClock.elapsedRealtime]), a `/proc/stat` `btime` sora
 * pedig a valódi boot-időbélyeg. Ezeket session-induláskor (minden
 * [bindings] hívásnál) újragenráljuk, hogy a valósághoz közeli adatot adjanak
 * — a load average-re viszont nincs Android API, az becsült marad.
 *
 * @author kiva
 */
object ProotSysData {

  private const val KERNEL_RELEASE = "6.17.0-neoterm"
  private const val KERNEL_VERSION = "#1 SMP PREEMPT_DYNAMIC Fri, 10 Oct 2025 00:00:00 +0000"

  private const val LOADAVG = "0.12 0.07 0.02 2/165 765\n"
  private const val OVERFLOW_ID = "65534\n"
  private const val CAP_LAST_CAP = "40\n"
  private const val MAX_USER_WATCHES = "4096\n"

  private val VERSION =
    "Linux version $KERNEL_RELEASE (proot@neoterm) " +
      "(gcc (GCC) 13.3.0, GNU ld (GNU Binutils) 2.42) $KERNEL_VERSION\n"

  private val STAT = """
cpu  1957 0 2877 93280 262 342 254 87 0 0
cpu0 31 0 226 12027 82 10 4 9 0 0
cpu1 45 0 664 11144 21 263 233 12 0 0
cpu2 494 0 537 11283 27 10 3 8 0 0
cpu3 359 0 234 11723 24 26 5 7 0 0
cpu4 295 0 268 11772 10 12 2 12 0 0
cpu5 270 0 251 11833 15 3 1 10 0 0
cpu6 430 0 520 11386 30 8 1 12 0 0
cpu7 30 0 172 12108 50 8 1 13 0 0
intr 127541 38 290 0 0 0 0 4 0 1 0 0 25329 258 0 5777 277 0 0 0 0 0 0 0 0 0
ctxt 140223
btime 1680020856
processes 772
procs_running 2
procs_blocked 0
softirq 75663 0 5903 6 25375 10774 0 243 11685 0 21677
""".trimStart()

  private val VMSTAT = """
nr_free_pages 1743136
nr_zone_inactive_anon 179281
nr_zone_active_anon 7183
nr_zone_inactive_file 22858
nr_zone_active_file 51328
nr_zone_unevictable 642
nr_zone_write_pending 0
nr_mlock 0
nr_bounce 0
nr_zspages 0
nr_free_cma 0
numa_hit 1259626
numa_miss 0
numa_foreign 0
numa_interleave 720
numa_local 1259626
numa_other 0
nr_inactive_anon 179281
nr_active_anon 7183
nr_inactive_file 22858
nr_active_file 51328
nr_unevictable 642
nr_slab_reclaimable 8091
nr_slab_unreclaimable 7804
nr_isolated_anon 0
nr_isolated_file 0
workingset_nodes 0
workingset_refault_anon 0
workingset_refault_file 0
workingset_activate_anon 0
workingset_activate_file 0
workingset_restore_anon 0
workingset_restore_file 0
workingset_nodereclaim 0
nr_anon_pages 7723
nr_mapped 8905
nr_file_pages 253569
nr_dirty 0
nr_writeback 0
nr_writeback_temp 0
nr_shmem 178741
nr_shmem_hugepages 0
nr_shmem_pmdmapped 0
nr_file_hugepages 0
nr_file_pmdmapped 0
nr_anon_transparent_hugepages 1
nr_vmscan_write 0
nr_vmscan_immediate_reclaim 0
nr_dirtied 0
nr_written 0
nr_kernel_misc_reclaimable 0
nr_foll_pin_acquired 0
nr_foll_pin_released 0
nr_kernel_stack 2780
nr_page_table_pages 344
nr_swapcached 0
pgpgin 890508
pgpgout 0
pswpin 0
pswpout 0
pgalloc_dma 272
pgalloc_dma32 261
pgalloc_normal 1328079
pgalloc_movable 0
pgfree 3077011
pgactivate 0
pgdeactivate 0
pgfault 176973
pgmajfault 488
pgrefill 0
pgreuse 19230
pgsteal_kswapd 0
pgsteal_direct 0
pgscan_kswapd 0
pgscan_direct 0
pgscan_direct_throttle 0
pginodesteal 0
slabs_scanned 0
kswapd_inodesteal 0
pageoutrun 0
pgrotated 0
drop_pagecache 0
drop_slab 0
oom_kill 0
pgmigrate_success 0
pgmigrate_fail 0
compact_stall 0
compact_fail 0
compact_success 0
unevictable_pgs_culled 27002
unevictable_pgs_scanned 0
unevictable_pgs_rescued 744
unevictable_pgs_mlocked 744
unevictable_pgs_munlocked 744
unevictable_pgs_cleared 0
unevictable_pgs_stranded 0
""".trimStart()

  /**
   * Biztosítja a fake fájlokat, és visszaadja a szükséges proot bind-párokat
   * (fakePath → realProcPath) azokra a `/proc` célokra, amik az adott eszközön
   * olvashatatlanok. A valódi, olvasható `/proc` célokat (pl. élő `/proc/stat`)
   * érintetlenül hagyjuk, hogy a valós CPU-adat átjöjjön.
   */
  fun bindings(): List<Pair<String, String>> {
    val dir = File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata")
    dir.mkdirs()

    // real /proc cél, sysdata fájlnév, tartalom, dinamikus-e (minden indításnál újraírjuk)
    val entries = listOf(
      Entry("/proc/loadavg", "loadavg", LOADAVG, dynamic = false),
      Entry("/proc/stat", "stat", statContent(), dynamic = true),
      Entry("/proc/uptime", "uptime", uptimeContent(), dynamic = true),
      Entry("/proc/version", "version", VERSION, dynamic = false),
      Entry("/proc/vmstat", "vmstat", VMSTAT, dynamic = false),
      Entry("/proc/sys/kernel/cap_last_cap", "sysctl_entry_cap_last_cap", CAP_LAST_CAP, dynamic = false),
      Entry("/proc/sys/fs/inotify/max_user_watches", "sysctl_inotify_max_user_watches", MAX_USER_WATCHES, dynamic = false),
      Entry("/proc/sys/kernel/overflowuid", "sysctl_kernel_overflowuid", OVERFLOW_ID, dynamic = false),
      Entry("/proc/sys/kernel/overflowgid", "sysctl_kernel_overflowgid", OVERFLOW_ID, dynamic = false)
    )

    val binds = ArrayList<Pair<String, String>>(entries.size)
    for (e in entries) {
      val fake = File(dir, e.name)
      // A dinamikus fájlokat (uptime, stat) minden session-indításnál frissítjük,
      // hogy a valódi eszköz-uptime-ot / boot-időt tükrözzék; a statikusakat csak egyszer.
      if (e.dynamic || !fake.exists()) {
        runCatching { fake.writeText(e.content) }
      }
      if (fake.exists() && needsFake(e.real)) {
        binds.add(fake.absolutePath to e.real)
      }
    }
    return binds
  }

  private data class Entry(
    val real: String,
    val name: String,
    val content: String,
    val dynamic: Boolean
  )

  /**
   * Valós `/proc/uptime` tartalom Android API-ból: első mező az eszköz tényleges
   * uptime-ja másodpercben ([SystemClock.elapsedRealtime], mély alvással együtt),
   * a második az idle-idő közelítése (uptime × magszám). A fájl statikus, így a
   * session indulásakor pontos, és menet közben nem „ketyeg" tovább.
   */
  private fun uptimeContent(): String {
    val up = SystemClock.elapsedRealtime() / 1000.0
    val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
    val idle = up * cores * 0.93
    return "%.2f %.2f\n".format(up, idle)
  }

  /** A `/proc/stat` valódi `btime` (boot epoch) sorral — a beégetett dátum helyett. */
  private fun statContent(): String {
    val btime = (System.currentTimeMillis() - SystemClock.elapsedRealtime()) / 1000
    return STAT.replace(Regex("(?m)^btime .*$"), "btime $btime")
  }

  /**
   * Csak akkor fake-elünk, ha a valódi /proc cél **olvashatatlan vagy üres**.
   *
   * A /proc/stat-ot szándékosan NEM cseréljük le pusztán a hiányzó `btime` sor
   * miatt: a statikus fake elölné az élő CPU-jiffie-ket, amitől a htop/top
   * CPU%-a (per-mag és per-process) `N/A`-ra esik. Ha a valódi /proc/stat
   * olvasható, hagyjuk átjönni az élő tartalmat (valós CPU%); a btime esetleges
   * hiánya csak egy kozmetikai `ps` „Unable to get system boot time" üzenet.
   * Ha viszont a kernel (SELinux) tiltja az olvasást, a fake lép életbe.
   */
  private fun needsFake(realPath: String): Boolean {
    return try {
      File(realPath).readText().isEmpty()
    } catch (e: Exception) {
      true
    }
  }
}
