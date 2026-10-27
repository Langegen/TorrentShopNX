#!/usr/bin/env bash
# Same result as build_libarchive_switch.sh (static libarchive with the 7z
# LZMA2 + ARM64 filter into _external/libarchive-switch), but built with the
# tarball's autotools ./configure instead of CMake.
#
# Use it when the devkitPro MSYS2 cmake/curl are broken (a partly upgraded
# MSYS2 makes them exit with 0xC0000139 / code 127 without a message). It
# expects the tarball already downloaded and verified next to the sources:
#   _external/libarchive-switch/src/libarchive-3.8.9.tar.gz
# build_libarchive_switch.sh downloads, checks and patches it; if its curl is
# broken, download the tarball by other means and run that script once (it
# skips the download, checks the SHA-256, extracts and patches, then stops at
# cmake), then run this one.
set -euo pipefail

LA_VERSION="3.8.9"
: "${DEVKITPRO:=/opt/devkitpro}"
PORTLIBS="${DEVKITPRO}/portlibs/switch"
export PATH="${DEVKITPRO}/devkitA64/bin:${PORTLIBS}/bin:${PATH}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/_external/libarchive-switch"
SRC="${OUT}/src/libarchive-${LA_VERSION}"

[ -x "${SRC}/configure" ] || { echo "sources not prepared: ${SRC} (run build_libarchive_switch.sh first)"; exit 1; }
grep -q 'not supported on Nintendo Switch' "${SRC}/libarchive/archive_read_support_filter_program.c" ||
    { echo "Switch patches missing in ${SRC} (run build_libarchive_switch.sh first)"; exit 1; }

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
export CC=aarch64-none-elf-gcc
export AR=aarch64-none-elf-gcc-ar
export RANLIB=aarch64-none-elf-gcc-ranlib
export CPPFLAGS="-D__SWITCH__ -I${PORTLIBS}/include -isystem ${DEVKITPRO}/libnx/include"
export CFLAGS="${ARCH} -O2 -ffunction-sections -fdata-sections"
export LDFLAGS="-specs=${DEVKITPRO}/libnx/switch.specs ${ARCH} -L${PORTLIBS}/lib -L${DEVKITPRO}/libnx/lib"
export LIBS="-lnx"

cd "${SRC}"
rm -rf _abuild && mkdir _abuild && cd _abuild
../configure --host=aarch64-none-elf --prefix="${OUT}" \
    --disable-shared --enable-static \
    --disable-bsdtar --disable-bsdcat --disable-bsdcpio --disable-bsdunzip \
    --disable-acl --disable-xattr \
    --without-xml2 --without-expat --without-openssl --without-mbedtls \
    --without-nettle --without-cng --without-libb2 --without-iconv \
    --with-lzma --with-zstd --with-zlib --with-bz2lib --with-lz4

grep -q '^#define HAVE_LIBLZMA 1' config.h ||
    { echo "liblzma was not detected by configure"; exit 1; }
grep -q '^#define HAVE_LZMA_FILTER_ARM64 1' config.h ||
    echo "note: configure did not report HAVE_LZMA_FILTER_ARM64 (libarchive checks the header directly)"

make -j4 libarchive.la

rm -rf "${OUT}/include" "${OUT}/lib"
mkdir -p "${OUT}/include" "${OUT}/lib"
cp ../libarchive/archive.h ../libarchive/archive_entry.h "${OUT}/include/"
cp .libs/libarchive.a "${OUT}/lib/libarchive.a"
echo "${LA_VERSION}" > "${OUT}/VERSION"
echo "libarchive ${LA_VERSION} for Switch (autotools) installed to ${OUT}"
