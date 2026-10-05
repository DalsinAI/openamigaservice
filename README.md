# openamigaservice

`openservice.device` hands work from an Amiga to a named service, such as
`opentls.key/1` for TLS key maths, and `media.decode/1` for datatypes.
Programs ask by name and do not know where the work runs:

- **On a board in this machine.** Every Dalsin AutoConfig board with the
  services block: the services card, CPU cores, FPU or TPU boards. On
  AmigaChrome (a PC, or our appliance) these are virtual and the work runs on
  the host's own CPU.
- **On a Cradle on the LAN.** A real Amiga with no such board asks over mDNS
  which Cradle offers the service, and uses it once paired.
- **Nowhere.** OPEN fails, and the program does the work itself on the 68k.

## What is here

| Path | What |
| --- | --- |
| `include/devices/openservice.h` | The device's API |
| `src/device.c` | `openservice.device`: boards' rings, LAN connections, one worker process |
| `src/mdns.c` | One mDNS question for `_amigachrome._tcp.local` |
| `src/nursery.c` | `Nursery`: lists the boards and the Cradles on the LAN, and their services |
| `src/servicetest.c` | `ServiceTest`: open, list and `echo/1` calls, checked and timed |
| `cradle/` | A Cradle's side for the LAN: mDNS advertising, and a host for `echo/1`, `opentls.key/1` and `media.decode/1` |
| `host/opentls_key.c` | `opentls.key/1` on the host (docs/OPENTLS_KEY.md) |
| `host/media_decode.c` | `media.decode/1` on the host: AVIF and HEIC pictures for openamigaimage's `heif.datatype` (docs/MEDIA_DECODE.md) |
| `opentls/` | An OpenSSL 3 provider for AmiSSL that sends a TLS handshake's key maths to `opentls.key/1`, and does it on the 68k when nothing answers |

The board's registers and rings, and the LAN framing, are in the AmigaChrome
project's `design/SERVICES_CARD.md` (v1); the emulator side is built there.

## opentls

A program using AmiSSL calls `opentls_amiga_open()` after opening AmiSSL.
When a board or a paired Cradle offers `opentls.key/1`, every handshake's
certificate checks, the server's handshake signature and the key exchange
go there; TLS itself, and the bulk encryption, stay on the Amiga.
`opentls/tlstest.c` runs the same provider on Linux against real sites:
every handshake to example.com, login.live.com (TLS 1.2), github.com,
wikipedia.org, google.com and others completes with all key work sent;
bad certificates still fail, a service that says "bad" fails every
handshake, and with no service the work is done locally.
`opentls/tlsprobe.c` times handshakes on the Amiga through curl.

## Pairing

The device uses only Cradles whose fingerprint (their mDNS `fp=` value) is
listed in `ENV:OpenService/Paired`, one per line (copy it to ENVARC: to keep
it). Nursery shows each Cradle's fingerprint state. The LAN connection is not
encrypted yet; that comes with pairing by code.

## Building

`./build.sh`, with the os32-gcc16 compiler (`OS32_GCC16`). Install
`openservice.device` in `DEVS:`.

The host side needs, on the Cradle: OpenSSL 3 for `opentls.key/1`, and
libavif and libheif with libheif's HEVC decoder plugin for `media.decode/1`
(on Ubuntu: `libavif-dev libheif-dev libheif-plugin-libde265`). Without the
plugin HEIC files are refused; AVIF still works. `host/test_media_decode.py`
checks the decoder with files made by `avifenc` and `heif-enc`.

Measured on AmigaOS 3.2.3 (AmigaChrome, OpenSocket) against a Cradle on
another PC: 500 `echo/1` calls, all correct, 2.2 ms each.

## Licence

MIT, Copyright (c) 2026 Dalsin Limited. See LICENSE.
