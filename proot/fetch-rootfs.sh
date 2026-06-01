#!/usr/bin/env bash
# fetch-rootfs.sh — disztró-rootfs tarball-ok letöltése + újracsomagolása a
# NeoTerm proot-runtime által várt elrendezésbe.
#
# A NeoTerm a setupkor a következő layoutból tölt (lásd Distro.kt):
#
#   <BASE_URL>/proot/<arch>/proot                  (statikus proot ELF)
#   <BASE_URL>/rootfs/<distro>/<arch>.tar.xz       (rootfs)
#   <BASE_URL>/rootfs/<distro>/<arch>.tar.xz.sha256
#
# ahol <arch> ∈ {aarch64, arm, x86_64}  (NeoTerm SetupHelper.determineArchName),
#       <distro> ∈ {ubuntu, alpine, kali, arch}.
#
# Ez a script az upstream disztró-rootfs-eket tölti le (mindegyiknek saját
# arch-elnevezése és tarball-formátuma van), majd EGYSÉGESEN .tar.xz-be
# csomagolja és sha256-ot számol. A kimenet feltölthető bármely statikus
# HTTP-hostra (GitHub Releases, S3, raw.githubusercontent...), amit a
# NeoTerm `DEFAULT_PROOT_SOURCE`-ként használ.
#
# Használat:
#   ./fetch-rootfs.sh <out_dir> [distro ...] [--arch aarch64,arm,x86_64]
# Példa:
#   ./fetch-rootfs.sh ./out ubuntu alpine --arch aarch64
set -euo pipefail

OUT_DIR="${1:?usage: fetch-rootfs.sh <out_dir> [distro ...] [--arch a,b]}"
shift || true

DISTROS=()
ARCHES="aarch64,arm,x86_64"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) ARCHES="$2"; shift 2 ;;
    --arch=*) ARCHES="${1#*=}"; shift ;;
    ubuntu|alpine|kali|arch) DISTROS+=("$1"); shift ;;
    *) echo "ismeretlen arg: $1" >&2; exit 2 ;;
  esac
done
[[ ${#DISTROS[@]} -eq 0 ]] && DISTROS=(ubuntu alpine kali arch)

mkdir -p "${OUT_DIR}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# Verzió-pinek
UBUNTU_RELEASE="${UBUNTU_RELEASE:-24.04}"        # 24.04 LTS (noble)
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"

# Egy könyvtár-index (HTML) tartalmából kiszedi a mintára illeszkedő LEGÚJABB
# fájlnevet (verzió szerint rendezve). Így nem kell pontverziót hardcode-olni
# (az upstream fájlnevek pl. ubuntu-base-24.04.4-... pont-verziót tartalmaznak,
# ami időről időre változik).
latest_in_dir() {
  local dir_url="$1" pattern="$2"
  curl -fsSL --retry 4 --retry-delay 2 "${dir_url}" \
    | grep -oE "${pattern}" | sort -V | uniq | tail -n1
}

# NeoTerm-arch → upstream-arch leképezés disztrónként. A NeoTerm aarch64/arm/
# x86_64 neveket használ; az upstream mindegyik mást:
#   ubuntu/kali : arm64 / armhf / amd64   (debian-style)
#   alpine      : aarch64 / armv7 / x86_64
#   arch        : aarch64 / armv7 (ALARM) ; x86_64 (bootstrap)
deb_arch()    { case "$1" in aarch64) echo arm64 ;; arm) echo armhf ;; x86_64) echo amd64 ;; esac; }
alpine_arch() { case "$1" in aarch64) echo aarch64 ;; arm) echo armv7 ;; x86_64) echo x86_64 ;; esac; }
alarm_arch()  { case "$1" in aarch64) echo aarch64 ;; arm) echo armv7 ;; x86_64) echo "" ;; esac; }

# Letölt egy tarballt (bármilyen tömörítés), kibontja egy friss rootfs-dir-be,
# majd determinisztikusan .tar.xz-be csomagolja a kimeneti útra + sha256.
repack() {
  local url="$1" out_xz="$2" decomp="$3"
  echo "[fetch] ${url}"
  local raw="${TMP}/dl"
  curl -fL --retry 4 --retry-delay 2 -o "${raw}" "${url}"
  local rootfs="${TMP}/rootfs"
  rm -rf "${rootfs}"; mkdir -p "${rootfs}"
  case "${decomp}" in
    gz)  tar -C "${rootfs}" -xzf "${raw}" ;;
    xz)  tar -C "${rootfs}" -xJf "${raw}" ;;
    zst) tar -C "${rootfs}" --use-compress-program=unzstd -xf "${raw}" ;;
    *)   tar -C "${rootfs}" -xf "${raw}" ;;
  esac
  # Néhány bootstrap (Arch x86_64) egy extra felső szintű könyvtárba bont
  # (root.x86_64/). Ha pontosan egy dir van és nincs /etc a tetején, lépjünk be.
  if [[ ! -d "${rootfs}/etc" ]]; then
    local only
    only="$(find "${rootfs}" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    if [[ -n "${only}" && -d "${only}/etc" ]]; then
      rootfs="${only}"
    fi
  fi
  mkdir -p "$(dirname "${out_xz}")"
  # GNU tar: --numeric-owner hogy a proot --link2symlink alatt is konzisztens
  # legyen; XZ -9 a méretért.
  tar -C "${rootfs}" --numeric-owner -cpf - . | xz -9 -T0 > "${out_xz}"
  ( cd "$(dirname "${out_xz}")" && sha256sum "$(basename "${out_xz}")" > "$(basename "${out_xz}").sha256" )
  ls -lh "${out_xz}"
}

IFS=, read -ra ARCH_LIST <<< "${ARCHES}"

for distro in "${DISTROS[@]}"; do
  for arch in "${ARCH_LIST[@]}"; do
    out="${OUT_DIR}/rootfs/${distro}/${arch}.tar.xz"
    case "${distro}" in
      ubuntu)
        a="$(deb_arch "${arch}")"
        # Ubuntu base image a cdimage.ubuntu.com-on; a fájlnév a teljes
        # pont-verziót hordozza (ubuntu-base-24.04.4-base-arm64.tar.gz), ezért
        # a könyvtár-indexből szedjük ki a legújabbat.
        base="https://cdimage.ubuntu.com/ubuntu-base/releases/${UBUNTU_RELEASE}/release/"
        file="$(latest_in_dir "${base}" "ubuntu-base-[0-9.]+-base-${a}\.tar\.gz")"
        [[ -z "${file}" ]] && { echo "[fetch] HIBA: nincs ubuntu-base (${a}) itt: ${base}" >&2; exit 8; }
        repack "${base}${file}" "${out}" gz ;;
      alpine)
        a="$(alpine_arch "${arch}")"
        # A minirootfs fájlnév pont-verziót tartalmaz (3.20.x) — a branch
        # releases-könyvtárából a legújabbat vesszük.
        base="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/${a}/"
        file="$(latest_in_dir "${base}" "alpine-minirootfs-[0-9.]+-${a}\.tar\.gz")"
        [[ -z "${file}" ]] && { echo "[fetch] HIBA: nincs alpine-minirootfs (${a}) itt: ${base}" >&2; exit 8; }
        repack "${base}${file}" "${out}" gz ;;
      kali)
        a="$(deb_arch "${arch}")"
        url="https://kali.download/nethunter-images/current/rootfs/kali-nethunter-rootfs-minimal-${a}.tar.xz"
        repack "${url}" "${out}" xz ;;
      arch)
        a="$(alarm_arch "${arch}")"
        if [[ -z "${a}" ]]; then
          # Arch x86_64: nincs ALARM, a hivatalos bootstrap-ot használjuk.
          url="https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
          repack "${url}" "${out}" zst
        else
          url="http://os.archlinuxarm.org/os/ArchLinuxARM-${a}-latest.tar.gz"
          repack "${url}" "${out}" gz
        fi ;;
    esac
  done
done

echo "[fetch] kész. Kimenet: ${OUT_DIR}/rootfs/<distro>/<arch>.tar.xz"
echo "[fetch] Ne feledd a proot bináris(oka)t a ${OUT_DIR}/proot/<arch>/proot alá tenni (build-proot.sh)."
