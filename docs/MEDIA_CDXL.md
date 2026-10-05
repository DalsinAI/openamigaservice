# media.cdxl/1

Turns any movie FFmpeg reads (MP4, MKV, AVI, WMV, WebM, MPEG, MOV, FLV...)
into CDXL, the Amiga's own streaming video format, made for the machine that
will play it. It is an option, not something every video goes through:
openvideo.datatype still plays movies as they are, through `media.decode/1`.
CDXL is for playing without a Cradle afterwards, on a CDTV, CD32 or any
Amiga with a CDXL player, or for putting a movie on a CD or a disk.

Host code: `host/cdxl.c`, built into `host/libmediadecode.so` (entry
`cx_call`, the same arguments as `md_call`) when FFmpeg's headers are there.
On the Amiga: `ToCDXL` (`src/tocdxl.c`). On the Cradle's own command line:
`host/media_cdxl.py MOVIE OUT.cdxl --preset aga`.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | CONVERT | buf0 the movie; `extra` the settings (below); buf1 (out) 24 bytes of info | result a handle, aux the CDXL's bytes |
| 2 | READ | `arg` the handle; `extra[0]` the offset; buf1 (out) room for a piece | result the bytes written (0 at the end) |
| 3 | CLOSE | `arg` the handle | |

Settings:

- `extra[0]` the preset (below).
- `extra[1]` the largest picture, width << 16 | height (0: the preset's). The
  movie keeps its shape inside it; the width is a multiple of 16.
- `extra[2]` bits 0-7 frames a second, bits 8-15 bitplanes for ECS and AGA
  (1 to 8: 2 to 256 colours); 0: the preset's.
- `extra[3]` bits 0-17 the sound's rate (0: the preset's), bit 30 no sound,
  bit 31 stereo.

| Preset | `extra[0]` | Picture | Size | Frames a second | Sound |
| --- | --- | --- | --- | --- | --- |
| ECS | 0 | 32 colours, 5 bitplanes | 320 x 180 | 12 | 11 kHz mono |
| ECS HAM | 1 | HAM6 | 320 x 180 | 12 | 11 kHz mono |
| AGA | 2 | 256 colours, 8 bitplanes | 320 x 180 | 15 | 22 kHz mono |
| AGA HAM | 3 | HAM8 | 320 x 180 | 15 | 22 kHz mono |
| RTG | 4 | 24-bit chunky | 640 x 360 | 25 | 22 kHz stereo |

ToCDXL picks AGA on an AGA machine and ECS otherwise, when no PRESET is given.

Info (big-endian u32s): `'CDXL'`, frames, width, height, then bitplanes |
mode << 8 (0 colours, 1 HAM, 2 chunky) | stereo << 16, then frames a second
<< 24 | the sound's rate.

The file: each frame is a 32-byte header (type 1, the standard one), its
palette as 12-bit colours (2 bytes each; none for chunky), its picture as
bitplanes one after the other (rows padded to 16 bits) or as RGB bytes,
then its sound as signed 8-bit samples (left, then right). The header keeps
the sound's rate at bytes 24-25 and the frames a second at byte 26, as
FFmpeg's CDXL reader expects. Each frame's sound is an even number of
samples (Paula plays words), so the rate is rounded to a multiple of the
frame rate (11025 Hz at 12 frames a second becomes 11016 Hz).

Each frame gets its own palette by median cut, drawn with a 4x4 ordered
dither; HAM frames pick, pixel by pixel, the nearer of a base colour and the
last pixel with one channel changed. Frames are taken from the movie at the
CDXL's rate (the latest picture at each frame's time).

CONVERT does the whole movie before it answers, a fraction of a second for
a short clip and seconds for a long one, and keeps the result in the cache
(`$OPENSERVICE_CACHE/cdxl`, else `~/.cache/openservice/cdxl`), named by the
movie's hash and the settings, so asking again costs nothing. The host keeps
8 results open; the oldest goes first.

Status: 0; -2 for a file with no video FFmpeg can read, bad settings or a
handle that is not open; -4 for a buf1 too small; -5 when the host failed.

Measured in the cloud on a 3-second 640 x 360 clip: ECS 0.12 s, ECS HAM
0.18 s, AGA 0.24 s, AGA HAM 0.42 s, RTG 0.8 s. On a still picture, against
the original scaled to the same size: ECS 24 dB, ECS HAM 29 dB, AGA 28 dB,
AGA HAM 35 dB, RTG 52 dB. `host/test_media_cdxl.py` reads every preset back
with FFmpeg's own CDXL reader.

Not yet: 24-bit (type 0) palettes for AGA, which few players read; a
progress report during CONVERT; playing on real hardware (no CDXL player
has been tried yet).
