#!/usr/bin/env bash
# Cross-compile a minimal PulseAudio (+ libsndfile) for Android arm64-v8a with
# the NDK, for NeoTerm's embedded Android-side audio server.
#
# We build vanilla PulseAudio with its built-in modules only:
#   - module-native-protocol-tcp  → distro apps connect via PULSE_SERVER=:4713
#   - module-pipe-sink            → writes raw PCM to a FIFO that NeoTerm's
#                                    AudioTrack bridge plays (no custom C sink
#                                    module needed).
#
# Output layout (under $OUT):
#   bin/pulseaudio
#   lib/*.so                (libpulse*, libsndfile, …)
#   lib/pulseaudio/modules/*.so
# These get packaged into the APK and run as the app uid (no root, no proot).
#
# Usage: build-pulseaudio.sh <out-dir> [--api 26] [--abi arm64-v8a]
set -euo pipefail

OUT="${1:?output dir required}"; shift || true
API=26
ABI=arm64-v8a
PA_VERSION="${PA_VERSION:-17.0}"
SNDFILE_VERSION="${SNDFILE_VERSION:-1.2.2}"

while [ $# -gt 0 ]; do
  case "$1" in
    --api) API="$2"; shift 2;;
    --abi) ABI="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must point at the NDK}"

# Directory of this script — used to find our bundled module sources (the
# AAudio *source*/microphone module, which upstream Termux doesn't ship).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WORK="$(mktemp -d)"
PREFIX="$WORK/prefix"
mkdir -p "$PREFIX" "$OUT"

HOST_TAG=linux-x86_64
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"
TARGET=aarch64-linux-android

export CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"
export CXX="$TOOLCHAIN/bin/${TARGET}${API}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

# Robust download: try each URL with retries/backoff and reject tiny error pages.
# GNU's ftpmirror.gnu.org sometimes redirects to an HTML error instead of the
# tarball, which then fails to untar ("This does not look like a tar archive").
fetch() {
  local out="$1"; shift
  local minsize="${MIN_FETCH_SIZE:-1024}"
  local url tries
  for url in "$@"; do
    for tries in 1 2 3 4; do
      if curl -fSL --retry 3 --retry-delay 2 --connect-timeout 20 -o "$out" "$url" \
         && [ "$(stat -c%s "$out" 2>/dev/null || echo 0)" -ge "$minsize" ]; then
        return 0
      fi
      echo "  download failed ($url), attempt $tries" >&2
      sleep $((tries * 2))
    done
  done
  echo "ERROR: could not download $out from: $*" >&2
  return 1
}

echo "── libsndfile $SNDFILE_VERSION (cmake/NDK) ──"
fetch "$WORK/sndfile.tar.xz" \
  "https://github.com/libsndfile/libsndfile/releases/download/${SNDFILE_VERSION}/libsndfile-${SNDFILE_VERSION}.tar.xz"
tar -C "$WORK" -xf "$WORK/sndfile.tar.xz"
cmake -S "$WORK/libsndfile-${SNDFILE_VERSION}" -B "$WORK/sndfile-build" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF \
  -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build "$WORK/sndfile-build" --target install -j"$(nproc)"

echo "── stub libintl (bionic has no gettext/libintl) ──"
# PulseAudio requires libintl/dgettext for NLS, absent on Android. Provide a
# passthrough stub so it links; translations just return the original strings.
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cat > "$WORK/libintl.h" <<'H'
#ifndef _LIBINTL_H
#define _LIBINTL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
char *gettext(const char *msgid);
char *dgettext(const char *domainname, const char *msgid);
char *dcgettext(const char *domainname, const char *msgid, int category);
char *ngettext(const char *msgid1, const char *msgid2, unsigned long int n);
char *dngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n);
char *dcngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n, int category);
char *textdomain(const char *domainname);
char *bindtextdomain(const char *domainname, const char *dirname);
char *bind_textdomain_codeset(const char *domainname, const char *codeset);
#ifdef __cplusplus
}
#endif
#endif
H
cat > "$WORK/libintl.c" <<'C'
char *gettext(const char *m){return (char*)m;}
char *dgettext(const char *d,const char *m){(void)d;return (char*)m;}
char *dcgettext(const char *d,const char *m,int c){(void)d;(void)c;return (char*)m;}
char *ngettext(const char *m1,const char *m2,unsigned long int n){return (char*)(n==1?m1:m2);}
char *dngettext(const char *d,const char *m1,const char *m2,unsigned long int n){(void)d;return (char*)(n==1?m1:m2);}
char *dcngettext(const char *d,const char *m1,const char *m2,unsigned long int n,int c){(void)d;(void)c;return (char*)(n==1?m1:m2);}
char *textdomain(const char *d){return (char*)(d?d:"messages");}
char *bindtextdomain(const char *d,const char *dir){(void)d;return (char*)dir;}
char *bind_textdomain_codeset(const char *d,const char *cs){(void)d;return (char*)cs;}
C
cp "$WORK/libintl.h" "$PREFIX/include/libintl.h"
"$CC" -shared -fPIC -o "$PREFIX/lib/libintl.so" "$WORK/libintl.c"

# PulseAudio loads modules via libltdl, absent on Android — cross-build it.
LIBTOOL_VERSION="${LIBTOOL_VERSION:-2.4.7}"
echo "── libltdl $LIBTOOL_VERSION (autotools/NDK) ──"
fetch "$WORK/libtool.tar.gz" \
  "https://ftp.gnu.org/gnu/libtool/libtool-${LIBTOOL_VERSION}.tar.gz" \
  "https://mirrors.kernel.org/gnu/libtool/libtool-${LIBTOOL_VERSION}.tar.gz" \
  "https://ftpmirror.gnu.org/libtool/libtool-${LIBTOOL_VERSION}.tar.gz"
tar -C "$WORK" -xf "$WORK/libtool.tar.gz"
(
  cd "$WORK/libtool-${LIBTOOL_VERSION}"
  ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
    --enable-ltdl-install --enable-shared --disable-static \
    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" CFLAGS="-fPIC"
  # libltdl is built from the top-level Makefile (libtool itself is shell, so
  # the only real compilation here is libltdl). Build + install everything into
  # the prefix; we only consume libltdl.so + ltdl.h.
  make -j"$(nproc)"
  make install
)

echo "── PulseAudio $PA_VERSION (meson/NDK) ──"
fetch "$WORK/pa.tar.xz" \
  "https://www.freedesktop.org/software/pulseaudio/releases/pulseaudio-${PA_VERSION}.tar.xz" \
  "https://freedesktop.org/software/pulseaudio/releases/pulseaudio-${PA_VERSION}.tar.xz"
tar -C "$WORK" -xf "$WORK/pa.tar.xz"
PA_SRC="$WORK/pulseaudio-${PA_VERSION}"

# Android seccomp kills setresgid/setresuid (SIGSYS), which pa_drop_root() calls
# unconditionally. We never run SUID-root, so make it a no-op.
sed -i 's|^void pa_drop_root(void) {|void pa_drop_root(void) { return; /* NeoTerm: Android seccomp */|' \
  "$PA_SRC/src/daemon/caps.c"

# Add Termux's proven OpenSL ES sink — a properly-clocked native Android sink
# (PA → OpenSL → speaker), so we don't need the open-loop pipe-sink + FIFO +
# AudioTrack (which drifted and underran).
echo "── add OpenSL ES + AAudio sink/source modules ──"
mkdir -p "$PA_SRC/src/modules/sles" "$PA_SRC/src/modules/aaudio"
MIN_FETCH_SIZE=256 fetch "$PA_SRC/src/modules/sles/module-sles-sink.c" \
  "https://raw.githubusercontent.com/termux/termux-packages/master/packages/pulseaudio/module-sles-sink.c"
MIN_FETCH_SIZE=256 fetch "$PA_SRC/src/modules/aaudio/module-aaudio-sink.c" \
  "https://raw.githubusercontent.com/termux/termux-packages/master/packages/pulseaudio/module-aaudio-sink.c"
# Termux ships no AAudio source — use our own (microphone capture). OpenSL's
# source would drag in libOpenSLES→libinput→libtinyxml2, which doesn't resolve
# on some devices (same reason we dropped the OpenSL sink), so AAudio only.
cp "$SCRIPT_DIR/module-aaudio-source.c" "$PA_SRC/src/modules/aaudio/module-aaudio-source.c"
sed -i "/^all_modules = \[/a\\  [ 'module-sles-sink', 'sles/module-sles-sink.c', [], [], [cc.find_library('OpenSLES', required: true)] ],\n  [ 'module-aaudio-sink', 'aaudio/module-aaudio-sink.c', [], [], [cc.find_library('aaudio', required: true)] ],\n  [ 'module-aaudio-source', 'aaudio/module-aaudio-source.c', [], [], [cc.find_library('aaudio', required: true)] ]," \
  "$PA_SRC/src/modules/meson.build"

cat > "$WORK/android-cross.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-I$PREFIX/include']
c_link_args = ['-L$PREFIX/lib', '-Wl,--undefined-version']
cpp_args = ['-I$PREFIX/include']
cpp_link_args = ['-L$PREFIX/lib', '-Wl,--undefined-version']
EOF

meson setup "$WORK/pa-build" "$PA_SRC" \
  --cross-file "$WORK/android-cross.ini" \
  --prefix=/ --libdir=lib --buildtype=release --default-library=shared \
  -Ddaemon=true -Dclient=true -Ddoxygen=false -Dman=false -Dtests=false \
  -Ddatabase=simple -Dalsa=disabled -Dasyncns=disabled -Davahi=disabled \
  -Dbluez5=disabled -Ddbus=disabled -Dfftw=disabled -Dglib=disabled \
  -Dgsettings=disabled -Dgtk=disabled -Dipv6=true -Djack=disabled \
  -Dlirc=disabled -Dopenssl=disabled -Dorc=disabled -Doss-output=disabled \
  -Dsamplerate=disabled -Dsoxr=disabled -Dspeex=disabled -Dsystemd=disabled \
  -Dtcpwrap=disabled -Dudev=disabled -Dx11=disabled -Dbashcompletiondir=no \
  -Dzshcompletiondir=no -Dudevrulesdir=no -Dadrian-aec=true -Dwebrtc-aec=disabled
# bionic's execinfo.h exists but doesn't declare backtrace() until API 33, and
# sys/capability.h exists without libcap (cap_t/cap_init). meson enabled both
# from header presence; drop them so PA uses its fallbacks.
sed -i '/define HAVE_EXECINFO_H/d;/define HAVE_SYS_CAPABILITY_H/d;/define HAVE_CAPABILITIES/d' \
  "$WORK/pa-build/config.h" || true

ninja -C "$WORK/pa-build"
DESTDIR="$WORK/pa-install" ninja -C "$WORK/pa-build" install

echo "── collect artifacts ──"
mkdir -p "$OUT/bin" "$OUT/lib/pulseaudio/modules"
cp "$WORK/pa-install/bin/pulseaudio" "$OUT/bin/" 2>/dev/null || \
  cp "$(find "$WORK/pa-install" -name pulseaudio -type f | head -n1)" "$OUT/bin/"
# All shared libraries that are NOT loadable modules (module-*.so) go to lib/
# (on LD_LIBRARY_PATH): libpulse*, libpulsecommon/core, the cross-built deps
# (libsndfile/libltdl/libintl), AND the module *helper* libs
# (libprotocol-native, libcli, librtp, …) which PA installs in the modules dir.
find "$WORK/pa-install" -name '*.so*' ! -name 'module-*' -exec cp -a {} "$OUT/lib/" \;
cp -a "$PREFIX"/lib/*.so* "$OUT/lib/" 2>/dev/null || true
# The loadable modules we actually use.
for m in module-native-protocol-tcp module-aaudio-sink module-aaudio-source \
         module-sles-sink module-pipe-sink module-null-sink module-simple-protocol-tcp; do
  find "$WORK/pa-install" -path '*/modules/*' -name "${m}.so" \
    -exec cp -a {} "$OUT/lib/pulseaudio/modules/" \; 2>/dev/null || true
done

"$STRIP" "$OUT/bin/pulseaudio" 2>/dev/null || true
find "$OUT/lib" -name '*.so*' -exec "$STRIP" {} \; 2>/dev/null || true

echo "── result ──"
ls -lR "$OUT" | sed -n '1,80p'
rm -rf "$WORK"
