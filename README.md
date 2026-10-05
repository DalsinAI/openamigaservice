# openamigaservice

`openservice.device` hands work from an Amiga to a named service, such as
`opentls.key/1` for TLS key maths, and later media decoding for datatypes.
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
| `cradle/` | A Cradle's side for the LAN: mDNS advertising, and a host for `echo/1` and `opentls.key/1` |
| `host/opentls_key.c` | `opentls.key/1` on the host (docs/OPENTLS_KEY.md) |

The board's registers and rings, and the LAN framing, are in the AmigaChrome
project's `design/SERVICES_CARD.md` (v1); the emulator side is built there.

## Pairing

The device uses only Cradles whose fingerprint (their mDNS `fp=` value) is
listed in `ENV:OpenService/Paired`, one per line (copy it to ENVARC: to keep
it). Nursery shows each Cradle's fingerprint state. The LAN connection is not
encrypted yet; that comes with pairing by code.

## Building

`./build.sh`, with the os32-gcc16 compiler (`OS32_GCC16`). Install
`openservice.device` in `DEVS:`.

Measured on AmigaOS 3.2.3 (AmigaChrome, OpenSocket) against a Cradle on
another PC: 500 `echo/1` calls, all correct, 2.2 ms each.

## Licence

MIT, Copyright (c) 2026 Dalsin Limited. See LICENSE.
