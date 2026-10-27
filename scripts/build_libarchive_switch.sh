#!/usr/bin/env bash
# Builds a static libarchive for Nintendo Switch into _external/libarchive-switch.
#
# Why: devkitPro's switch-libarchive 3.7.2 package was built (July 2024) against a
# liblzma without LZMA_FILTER_ARM64, so 7z folders packed as LZMA/LZMA2 + ARM64
# filter (7-Zip 23+ uses it for native AArch64 .so/.nro files) fail with
# "Unexpected codec ID: A". Rebuilding against the current switch-liblzma
# (>= 5.4) compiles that path in.
#
# Run inside the devkitPro MSYS2 shell (C:\devkitPro\msys2\usr\bin\bash.exe -l)
# or let build_switch.ps1 call it automatically when the library is missing.
set -euo pipefail

LA_VERSION="3.8.9"
LA_SHA256="f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4"
LA_URL="https://github.com/libarchive/libarchive/releases/download/v${LA_VERSION}/libarchive-${LA_VERSION}.tar.gz"

: "${DEVKITPRO:=/opt/devkitpro}"
PORTLIBS="${DEVKITPRO}/portlibs/switch"
export PATH="${DEVKITPRO}/devkitA64/bin:${PORTLIBS}/bin:${PATH}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/_external/libarchive-switch"
WORK="${OUT}/src"

# The whole point of this build: liblzma must know the ARM64 BCJ filter.
printf '#include <lzma.h>\n#ifndef LZMA_FILTER_ARM64\n#error liblzma has no ARM64 filter\n#endif\n' |
    aarch64-none-elf-gcc -I"${PORTLIBS}/include" -fsyntax-only -x c - ||
    { echo "switch-liblzma is too old (need >= 5.4). Run: dkp-pacman -S switch-liblzma"; exit 1; }

mkdir -p "${WORK}"
cd "${WORK}"
TARBALL="libarchive-${LA_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
    curl -fL -o "${TARBALL}.part" "${LA_URL}"
    mv "${TARBALL}.part" "${TARBALL}"
fi
echo "${LA_SHA256}  ${TARBALL}" | sha256sum -c -

rm -rf "libarchive-${LA_VERSION}"
tar xzf "${TARBALL}"
cd "libarchive-${LA_VERSION}"

# Switch fixes (same intent as devkitPro's libarchive-3.7.2.patch):
# newlib has no fchdir() (read-disk is unused here) and exposes _timezone, not timezone.
sed -i 's|^#error fchdir function required\.$|#ifndef __SWITCH__\n#error fchdir function required.\n#endif|' \
    libarchive/archive_read_disk_posix.c
sed -i 's|^#elif defined(__CYGWIN__) \|\| defined(__MINGW32__) \|\| defined(__BORLANDC__)$|& \|\| defined(__SWITCH__)|' \
    libarchive/archive_write_set_format_iso9660.c
grep -q 'defined(__BORLANDC__) || defined(__SWITCH__)' libarchive/archive_write_set_format_iso9660.c ||
    { echo "iso9660 timezone patch did not apply"; exit 1; }
# No fork()/waitpid() on Switch: the external-program filter (lrzip, grzip...)
# is replaced by a stub that reports an error.
PROGRAM_STUB='#ifdef __SWITCH__\nint\n__archive_read_program(struct archive_read_filter *f, const char *cmd)\n{\n\t(void)cmd;\n\tarchive_set_error(\&f->archive->archive, ARCHIVE_ERRNO_MISC,\n\t    "External programs are not supported on Nintendo Switch");\n\treturn (ARCHIVE_FATAL);\n}\n#else'
sed -i "s|^#include \"filter_fork.h\"\$|&\n${PROGRAM_STUB}|" libarchive/archive_read_support_filter_program.c
printf '\n#endif /* __SWITCH__ */\n' >> libarchive/archive_read_support_filter_program.c
grep -q 'not supported on Nintendo Switch' libarchive/archive_read_support_filter_program.c ||
    { echo "program filter patch did not apply"; exit 1; }

# Pass the Switch toolchain explicitly: an inherited CMAKE_TOOLCHAIN_FILE may point
# at the bare devkitA64 one, which drops -D__SWITCH__, -march and -mtp=soft.
aarch64-none-elf-cmake -B _build -S . -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${PORTLIBS}" \
    -DCMAKE_FIND_ROOT_PATH="${PORTLIBS}" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DCMAKE_INSTALL_PREFIX="${OUT}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_CAT=OFF -DENABLE_CPIO=OFF -DENABLE_TAR=OFF -DENABLE_UNZIP=OFF \
    -DENABLE_LZMA=ON -DENABLE_ZSTD=ON \
    -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=OFF \
    -DENABLE_LIBXML2=OFF -DENABLE_LIBB2=OFF -DENABLE_ICONV=OFF \
    -DENABLE_ACL=OFF -DENABLE_XATTR=OFF \
    -DENABLE_TEST=OFF -DBUILD_TESTING=OFF

grep -q '^#define HAVE_LIBLZMA 1' _build/config.h ||
    { echo "liblzma was not detected by libarchive's CMake"; exit 1; }

cmake --build _build --target archive_static

rm -rf "${OUT}/include" "${OUT}/lib"
mkdir -p "${OUT}/include" "${OUT}/lib"
cp libarchive/archive.h libarchive/archive_entry.h "${OUT}/include/"
cp _build/libarchive/libarchive.a "${OUT}/lib/libarchive.a"
echo "${LA_VERSION}" > "${OUT}/VERSION"

echo "libarchive ${LA_VERSION} for Switch installed to ${OUT}"
