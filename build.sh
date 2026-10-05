#!/bin/sh
# Builds openservice.device, Nursery and ServiceTest for AmigaOS 3.x with the
# os32-gcc16 compiler (GCC 16.2, libnix), and the host side's libraries: TLS keys, and media decoding.
#   OS32_GCC16  compiler root holding prefix/ (default ~/AmigaChrome/stoves/os32-gcc16)
# MIT, Copyright (c) 2026 Dalsin Limited.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
P=${OS32_GCC16:-"$HOME/AmigaChrome/stoves/os32-gcc16"}/prefix
CC="$P/bin/m68k-amigaos-gcc"
mkdir -p "$HERE/bin" "$HERE/obj"
cd "$HERE"
# The device: its own ROMTag, no C library start-up; start.c is linked first.
for f in start device mdns paired; do
    "$CC" -m68020 -Os -Wall -fomit-frame-pointer -Iinclude -c src/$f.c -o obj/$f.o
done
"$CC" -nostartfiles -m68020 -o bin/openservice.device obj/start.o obj/device.o obj/mdns.o obj/paired.o \
    -lamiga -Wl,-Map=obj/openservice.device.map
"$CC" -m68020 -mcrt=nix20 -Os -Wall -Iinclude -o bin/Nursery src/nursery.c src/mdns.c src/paired.c -lamiga
"$CC" -m68020 -mcrt=nix20 -Os -Wall -Iinclude -o bin/ServiceTest src/servicetest.c -lamiga
# The host side (Linux, OpenSSL 3), for the LAN Cradle and the emulator.
if [ -f /usr/include/openssl/evp.h ]; then
    gcc -O2 -Wall -fPIC -shared -o host/libopentlskey.so host/opentls_key.c -lcrypto
fi
# media.decode/1 (libavif, and libheif with its HEVC decoder plugin)
if [ -f /usr/include/avif/avif.h ] && [ -f /usr/include/libheif/heif.h ]; then
    gcc -O2 -Wall -fPIC -shared -o host/libmediadecode.so host/media_decode.c -lavif -lheif
fi
ls -l bin
