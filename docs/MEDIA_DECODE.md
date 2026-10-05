# media.decode/1

Decodes pictures and sounds for an Amiga's datatypes: the Amiga sends the
file, the host answers with 32-bit ARGB pixels scaled down to fit, or 16-bit
PCM at the rate and channels the Amiga can play. Host code:
`host/media_decode.c` for AVIF (libavif) and HEIC/HEIF (libheif), and
`host/media_av.c` for any sound FFmpeg reads (FLAC, Ogg Vorbis, Opus, MP3,
AAC, ALAC, WMA and more) and any video it reads (H.264, HEVC, AV1, VP8/VP9,
MPEG-4, MPEG-1/2, WMV, MJPEG, Theora).

The Amiga side is openamigaimage's datatypes (`heif.datatype`,
`opensound.datatype`, `openvideo.datatype`, and `webm.datatype` for its
sound). They
assume a services card, or PiStorm-class or AmigaChrome-class networking to
a Cradle, so the decoded pixels come back whole.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | PROBE | buf0 the file; `extra[0]`, `extra[1]` the largest width and height wanted (0: any); buf1 (out) 24 bytes of info | result width, aux height (the picture's own) |
| 2 | DECODE | `arg` the frame (0 the first); buf0 the file; `extra[0]`, `extra[1]` as PROBE; buf1 (out) the pixels | result width, aux height (of the pixels written) |

PROBE's info, six big-endian u32s: kind (1 picture), format (`'AVIF'` or
`'HEIC'`), flags (bit 0: alpha), frames, then the width and height DECODE
will write for the same `extra[0]` and `extra[1]`.

DECODE's pixels are rows of 4-byte A, R, G, B, no padding: width x height x
4 bytes, the layout picture.datatype's `PBPAFMT_ARGB` takes. A picture that
does not fit inside the largest size asked for is scaled down (averaging
each output pixel's area), keeping its shape; it is never scaled up.
Pictures without alpha have A = 255.

Status: 0; -2 for a file it cannot decode or a format it does not know; -4
when buf1 is too small (PROBE's info says the size to give).

Frames: an AVIF sequence or a HEIF file with several images reports them in
`frames`; DECODE's `arg` picks one. Rotation and mirroring in HEIF files are
applied; in AVIF files not yet.

## Sounds

A file that is not one of the pictures above and that FFmpeg recognises is
taken as a sound (its best audio stream).

| Op | Request | Answer |
| --- | --- | --- |
| PROBE | buf0 the file; `extra[0]` the most channels wanted (0: any, at most 2); `extra[1]` the highest rate (0: the file's); buf1 (out) 24 bytes of info | result the sample frames DECODE will give, aux the rate |
| DECODE | `arg` the first sample frame; buf0 the file; `extra` as PROBE; buf1 (out) the samples | result the sample frames written, aux the rate |

PROBE's info for a sound: kind (3 sound), format (`'FLAC'`, `'VORB'`,
`'OPUS'`, `'MP3 '`, `'AAC '`, `'ALAC'`, `'WMA '`, or `'SOUN'` for another),
flags (0), sample frames, rate, channels.

The rate is the file's, halved until it is at most `extra[1]` (44.1 kHz
becomes 22.05 kHz for a limit of 28000). Samples are 16-bit signed
big-endian, the channels interleaved. DECODE writes as many frames as buf1
holds from `arg` on, so a long sound can come in pieces; there is no
TOOSMALL for sounds (except a buf1 smaller than one frame).

## Video

A video is sent once and kept open on the host, so each frame costs only
its pixels. The host keeps at most 8 open (the oldest goes first); handles
belong to the host, not to one Amiga.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 3 | VOPEN | buf0 the file; `extra[0]`, `extra[1]` the largest width and height wanted; buf1 (out) 24 bytes of info | result the handle, aux frames a second x 1000 |
| 4 | VFRAME | `arg` the handle; `extra[0]` the frame (0 the first; past the end: the last); `extra[1]` 0 for 256 colours, 1 for 24-bit RGB; buf1 (out) the frame | result the frame given |
| 5 | VCLOSE | `arg` the handle | |

VOPEN's info: kind (2 animation), format (`'H264'`, `'HEVC'`, `'AV1 '`,
`'VP8 '`, `'VP9 '`, `'MPG4'`, `'MPG2'`, `'WMV '`, `'MJPG'`, `'THOR'`, or
`'VIDE'` for another), flags (bit 1: it has a sound track, which PROBE and
DECODE give as a sound), frames, then the width and height of each frame.

VFRAME in 256 colours is one byte a pixel, rows packed, in the 6x6x6
colour cube that openamigaimage's webm.datatype uses (index i < 216 is red
i / 36, green i / 6 % 6, blue i % 6, each step 51; 216 to 255 a grey ramp,
unused here), with 4x4 ordered dithering. In 24-bit it is R, G, B bytes.
Frames asked for in order are decoded straight on; going back, or jumping
more than 50 frames ahead, seeks to the key frame before.
