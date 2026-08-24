#!/bin/sh
# Builds the PC engine test on a Windows machine with devkitPro MSYS2.
# Requires: msys/gcc, msys/curl, msys/zlib (installed via pacman).
# mbedtls is vendored under pctest/deps/.
set -e
cd "$(dirname "$0")/.."

GCC=/c/devkitPro/msys2/usr/bin/gcc.exe

$GCC -O2 -g -Wall \
    -Ipctest/compat -Isource/engine -Iinclude/engine -Ipctest/deps/include \
    -L/c/devkitPro/msys2/mingw64/lib \
    source/engine/bencode.c \
    source/engine/torrent_meta.c \
    source/engine/udp_tracker.c \
    source/engine/bt_peer.c \
    source/engine/torrentfs.c \
    source/engine/magnet.c \
    source/engine/dht.c \
    source/engine/dhtclient.c \
    source/engine/engine.c \
    pctest/utpstub.c \
    pctest/test_bencode.c \
    -o pctest/test_bencode \
    -Lpctest/deps/lib \
    -lmbedcrypto -lmbedtls -lmbedx509 -lcurl -lz -lpthread

echo "OK -> pctest/test_bencode"
