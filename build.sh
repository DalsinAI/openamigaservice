#!/bin/sh
# Builds openservice.device, Nursery, ServiceTest and ToCDXL for AmigaOS 3.x with the
# os32-gcc16 compiler (GCC 16.2, libnix), and the host side's libraries: TLS keys, media decoding and documents.
#   OS32_GCC16  compiler root holding prefix/ (default ~/AmigaChrome/stoves/os32-gcc16)
# MIT, Copyright (c) 2026 Dalsin Limited.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
P=${OS32_GCC16:-"$HOME/AmigaChrome/stoves/os32-gcc16"}/prefix
CC="$P/bin/m68k-amigaos-gcc"
mkdir -p "$HERE/bin" "$HERE/obj"
cd "$HERE"
# The device: its own ROMTag, no C library start-up; start.c is linked first.
for f in start device mdns paired oscrypto osrandom; do
    "$CC" -m68020 -Os -fno-delete-null-pointer-checks -Wall -fomit-frame-pointer -Iinclude -c src/$f.c -o obj/$f.o
done
"$CC" -nostartfiles -m68020 -o bin/openservice.device obj/start.o obj/device.o obj/mdns.o obj/paired.o obj/oscrypto.o obj/osrandom.o \
    -lamiga -Wl,-Map=obj/openservice.device.map
"$CC" -m68020 -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/Nursery src/nursery.c src/mdns.c src/paired.c src/oscrypto.c src/osrandom.c -lamiga
"$CC" -m68020 -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/ServiceTest src/servicetest.c -lamiga
"$CC" -m68020 -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/ToCDXL src/tocdxl.c -lamiga
# The host side (Linux, OpenSSL 3), for the LAN Cradle and the emulator.
if [ -f /usr/include/openssl/evp.h ]; then
    gcc -O2 -Wall -fPIC -shared -o host/libopentlskey.so host/opentls_key.c -lcrypto
fi
# media.decode/1: pictures with libavif, and libheif with its HEVC decoder
# plugin; sounds, video and most pictures with FFmpeg when its headers are
# there; RAW, ImageMagick, MIDI and SID through those tools at run time.
if pkg-config --exists libavif libheif 2>/dev/null; then
    if pkg-config --exists libavformat libavcodec libswresample libswscale libavutil 2>/dev/null; then
        gcc -O2 -Wall -DMD_AV -fPIC -shared -o host/libmediadecode.so host/media_decode.c host/media_av.c host/media_tool.c host/media_svg.c host/cdxl.c host/hostrun.c -lpthread -lz -ldl -lm \
            $(pkg-config --cflags --libs libavif libheif libavformat libavcodec libswresample libswscale libavutil)
    else
        gcc -O2 -Wall -fPIC -shared -o host/libmediadecode.so host/media_decode.c host/media_tool.c host/media_svg.c host/hostrun.c -lpthread -lz -ldl -lm $(pkg-config --cflags --libs libavif libheif)
    fi
fi
# doc.render/1: needs LibreOffice and poppler's tools at run time only.
gcc -O2 -Wall -fPIC -shared -o host/libdocrender.so host/doc_render.c host/hostrun.c -lpthread -ldl
ls -l bin
