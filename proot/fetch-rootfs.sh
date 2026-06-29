#!/usr/bin/env bash
# fetch-rootfs.sh — disztró-rootfs tarball-ok TÜKRÖZÉSE a NeoTerm release-ekbe.
#
# A proot-distro (https://github.com/termux/proot-distro) mintájára NEM
# csomagolunk újra semmit: az upstream rootfs tarballt VÁLTOZATLANUL letöltjük
# és továbbadjuk. A kibontás az ESZKÖZÖN, az app-ban történik (ProotInstaller),
# ahol a device node-ok kihagyhatók és a könyvtárak írhatóan jönnek létre — így
# nincs szükség root-ra. (A korábbi CI-s `tar -x` újracsomagolás nem-root
# userként elhasalt device node-okon, setuid fájlokon és írásvédett könyvtárakon.)
#
# Kimenet: <out>/rootfs/<distro>/<arch>.<ext>  (+ .sha256), ahol az <ext> az
# upstream tömörítése (gz/xz). A client Distro.archiveExt ezt tükrözi.
#
# Használat:
#   ./fetch-rootfs.sh <out_dir> [distro ...] [--arch aarch64,arm,x86_64]
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

# Verzió-pinek
UBUNTU_RELEASE="${UBUNTU_RELEASE:-24.04}"        # 24.04 LTS (noble)
ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"

# Egy könyvtár-index (HTML) tartalmából kiszedi a mintára illeszkedő LEGÚJABB
# fájlnevet (verzió szerint rendezve). Így nem kell pontverziót hardcode-olni.
# Some mirrors (kali.download, ubuntu cdimage CDNs) 403 the default curl UA from
# CI runners; send a browser-ish UA. -L follows redirects to the actual mirror.
CURL_UA="${CURL_UA:-Mozilla/5.0 (X11; Linux aarch64) NeoTerm-CI}"

latest_in_dir() {
  local dir_url="$1" pattern="$2"
  curl -fsSL -A "${CURL_UA}" --retry 4 --retry-delay 2 "${dir_url}" \
    | grep -oE "${pattern}" | sort -V | uniq | tail -n1
}

# NeoTerm-arch → upstream-arch leképezés disztrónként.
deb_arch()    { case "$1" in aarch64) echo arm64 ;; arm) echo armhf ;; x86_64) echo amd64 ;; esac; }
alpine_arch() { case "$1" in aarch64) echo aarch64 ;; arm) echo armv7 ;; x86_64) echo x86_64 ;; esac; }
alarm_arch()  { case "$1" in aarch64) echo aarch64 ;; arm) echo armv7 ;; x86_64) echo "" ;; esac; }

# Letölti az upstream tarballt VÁLTOZATLANUL a kimeneti útra + sha256.
mirror() {
  local url="$1" out_file="$2"
  echo "[fetch] ${url}"
  mkdir -p "$(dirname "${out_file}")"
  curl -fL -A "${CURL_UA}" --retry 4 --retry-delay 2 -o "${out_file}" "${url}"
  ( cd "$(dirname "${out_file}")" && sha256sum "$(basename "${out_file}")" > "$(basename "${out_file}").sha256" )
  ls -lh "${out_file}"
}

IFS=, read -ra ARCH_LIST <<< "${ARCHES}"

for distro in "${DISTROS[@]}"; do
  for arch in "${ARCH_LIST[@]}"; do
    case "${distro}" in
      ubuntu)
        a="$(deb_arch "${arch}")"
        base="https://cdimage.ubuntu.com/ubuntu-base/releases/${UBUNTU_RELEASE}/release/"
        file="$(latest_in_dir "${base}" "ubuntu-base-[0-9.]+-base-${a}\.tar\.gz")"
        [[ -z "${file}" ]] && { echo "[fetch] HIBA: nincs ubuntu-base (${a}) itt: ${base}" >&2; exit 8; }
        mirror "${base}${file}" "${OUT_DIR}/rootfs/${distro}/${arch}.tar.gz" ;;
      alpine)
        a="$(alpine_arch "${arch}")"
        base="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/${a}/"
        file="$(latest_in_dir "${base}" "alpine-minirootfs-[0-9.]+-${a}\.tar\.gz")"
        [[ -z "${file}" ]] && { echo "[fetch] HIBA: nincs alpine-minirootfs (${a}) itt: ${base}" >&2; exit 8; }
        mirror "${base}${file}" "${OUT_DIR}/rootfs/${distro}/${arch}.tar.gz" ;;
      kali)
        a="$(deb_arch "${arch}")"
        url="https://kali.download/nethunter-images/current/rootfs/kali-nethunter-rootfs-minimal-${a}.tar.xz"
        mirror "${url}" "${OUT_DIR}/rootfs/${distro}/${arch}.tar.xz" ;;
      arch)
        a="$(alarm_arch "${arch}")"
        if [[ -z "${a}" ]]; then
          # Arch x86_64: nincs ALARM; a hivatalos bootstrap zst. (A client
          # jelenleg gz/xz-t kezel — x86_64-et a workflow nem épít.)
          url="https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
          mirror "${url}" "${OUT_DIR}/rootfs/${distro}/${arch}.tar.zst"
        else
          url="http://os.archlinuxarm.org/os/ArchLinuxARM-${a}-latest.tar.gz"
          mirror "${url}" "${OUT_DIR}/rootfs/${distro}/${arch}.tar.gz"
        fi ;;
    esac
  done
done

echo "[fetch] kész. Kimenet: ${OUT_DIR}/rootfs/<distro>/<arch>.<ext> (upstream, változatlan)."
echo "[fetch] A kibontás az eszközön, a NeoTerm ProotInstaller-ben történik."
