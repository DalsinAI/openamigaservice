# media.decode/1

Decodes pictures and sounds for an Amiga's datatypes: the Amiga sends the
file, the host answers with 32-bit ARGB pixels scaled down to fit, or 16-bit
PCM at the rate and channels the Amiga can play. Host code:
`host/media_decode.c` for AVIF (libavif) and HEIC/HEIF (libheif);
`host/media_av.c` for pictures FFmpeg reads (JPEG, PNG, GIF, WebP, JPEG XL,
QOI, EXR, HDR, TIFF, DDS, JPEG 2000...), any sound it reads (FLAC, Ogg
Vorbis, Opus, MP3, AAC, ALAC, WMA, tracker modules through libopenmpt, and
more) and any video it reads (H.264, HEVC, AV1, VP8/VP9, MPEG-4, MPEG-1/2,
WMV, MJPEG, Theora); and `host/media_tool.c` for what host tools do better:
camera RAW (LibRaw's `dcraw_emu`, else `dcraw`), every other picture
ImageMagick reads (PSD, XCF, TGA, PCX, ...), MIDI (FluidSynth) and SID
tunes (sidplayfp).

The Amiga side is openamigaimage's datatypes (`heif.datatype`,
`opensound.datatype`, `openvideo.datatype`, and `webm.datatype` for its
sound). They
assume a services card, or PiStorm-class or AmigaChrome-class networking to
a Cradle, so the decoded pixels come back whole.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | PROBE | buf0 the file; `extra[0]`, `extra[1]` the largest width and height wanted (0: any); `extra[2]` the file's extension as a hint (0: none); `extra[3]` bit 0 `MD_EXACT`, for SVG (below); buf1 (out) 24 bytes of info | result width, aux height (the picture's own) |
| 2 | DECODE | `arg` the frame (0 the first); buf0 the file; `extra[0..3]` as PROBE; buf1 (out) the pixels | result width, aux height (of the pixels written) |

PROBE's info, six big-endian u32s: kind (1 picture), format, flags (bit 0:
alpha), frames, then the width and height DECODE will write for the same
`extra[0]` and `extra[1]`. Formats: `'AVIF'`, `'HEIC'`, `'JPEG'`, `'PNG '`,
`'GIF '`, `'WEBP'`, `'JXL '`, `'EXR '`, `'HDR '`, `'PSD '`, `'QOI '`,
`'DDS '`, `'J2K '`, `'TIFF'`, `'DPX '`, `'PCX '`, `'SGI '`, `'ICO '`, `'SVG '`, `'STIL'` (another
picture FFmpeg reads), `'RAW '` (camera RAW), or for ImageMagick the hint in
capitals (`'TGA '`), else `'IMGK'`.

Full size: `extra[0]` = `extra[1]` = 0 gives the picture at its own size,
for a browser that lays out and scales pictures itself. On the Cradle a
320x240 JPEG or WebP takes about 1.5 ms, a PNG about 5 ms, before the trip.

The hint is the extension in up to four ASCII letters, big-endian and
space-padded (`'CR2 '`, `'TGA '`). Pictures are recognised by their bytes
first; the hint matters for formats with no signature (TGA) and sends
camera RAW, which is TIFF inside, to LibRaw. GIF and APNG answer with their
first frame (and VOPEN plays them as a video without sound), an icon file
(`'ICO '`) with its largest icon, and a TrueType or OpenType font (hint
`'TTF '` or `'OTF '`) with ImageMagick's sample sheet. OpenRaster (`'ORA '`) and Krita (`'KRA '`) files answer with
their flattened picture, and a comic book (`'CBZ '`, only with the hint
`'CBZ '`) with its first page by name. Pictures through ImageMagick or LibRaw are converted once and
kept in the cache (`$OPENSERVICE_CACHE/picture`, else
`~/.cache/openservice/picture`), so a PROBE then a DECODE costs one
conversion. EXIF rotation is not applied (as the Amiga's own JPEG datatype).

DECODE's pixels are rows of 4-byte A, R, G, B, no padding: width x height x
4 bytes, the layout picture.datatype's `PBPAFMT_ARGB` takes. A picture that
does not fit inside the largest size asked for is scaled down (averaging
each output pixel's area), keeping its shape; it is never scaled up.
Pictures without alpha have A = 255.

Status: 0; -2 for a file it cannot decode or a format it does not know; -4
when buf1 is too small (PROBE's info says the size to give).

## SVG

SVG and SVGZ (format `'SVG '`, always with alpha) are drawn by librsvg and
cairo straight at the size wanted, not decoded and shrunk: a 16x16 icon at
64x64 takes well under a millisecond once loaded. The host opens
`librsvg-2.so.2` when first needed, so no development package is built
against; without it, SVG goes to ImageMagick like any other picture.

An SVG's own size (PROBE's result and aux) is the browser's: its `width` and
`height` in pixels at 96 dots an inch, else its `viewBox`'s size (one of
`width` and `height` with the `viewBox`'s aspect), else 300 x 150.

`extra[3]` bit 0 (`MD_EXACT`) asks for exactly `extra[0]` x `extra[1]`, as a
browser draws an `<img>` with a width and height: the drawing is placed in
that box by its `preserveAspectRatio` (centred and fitted by default,
stretched for `none`), transparent around it. One of them 0 keeps the SVG's
aspect; both 0 gives its own size. Sides are capped at 4096, keeping the
shape. An SVG without a `viewBox` is scaled as a whole and centred. Without
`MD_EXACT`, `extra[0..1]` are the largest size, as for any picture, so a
datatype's 4096 x 4096 never blows an icon up. SVGZ needs the hint `'SVGZ'`
(or `'SVG '`); a text SVG is recognised by an `<svg` element in its first
4 KB.

Frames: an AVIF sequence or a HEIF file with several images reports them in
`frames`; DECODE's `arg` picks one. Rotation and mirroring in HEIF files are
applied; in AVIF files not yet.

## Sounds

A file that is not one of the pictures above and that FFmpeg recognises is
taken as a sound (its best audio stream). MIDI files (`MThd`, or RIFF
`RMID`) are played through FluidSynth with `$OPENSERVICE_SOUNDFONT`, else
the General MIDI font in `/usr/share/sounds/sf2`; SID tunes (`PSID`,
`RSID`) through sidplayfp for `$OPENSERVICE_SIDSECONDS` (default 60), as
SID tunes never end. Both are rendered once to a WAV in the cache
(`.../sound`), then answered as below.

| Op | Request | Answer |
| --- | --- | --- |
| PROBE | buf0 the file; `extra[0]` the most channels wanted (0: any, at most 2); `extra[1]` the highest rate (0: the file's); buf1 (out) 24 bytes of info | result the sample frames DECODE will give, aux the rate |
| DECODE | `arg` the first sample frame; buf0 the file; `extra` as PROBE; buf1 (out) the samples | result the sample frames written, aux the rate |

PROBE's info for a sound: kind (3 sound), format (`'FLAC'`, `'VORB'`,
`'OPUS'`, `'MP3 '`, `'AAC '`, `'ALAC'`, `'WMA '`, `'MOD '` for tracker
modules, `'GME '` for console tunes through Game Music Emu, `'MIDI'`,
`'SID '`, or `'SOUN'` for another),
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
DECODE give as a sound; bit 2: an animation, GIF or APNG, that asks to play
over and over), frames, then the width and height of each frame.

VFRAME in 256 colours is one byte a pixel, rows packed, in the 6x6x6
colour cube that openamigaimage's webm.datatype uses (index i < 216 is red
i / 36, green i / 6 % 6, blue i % 6, each step 51; 216 to 255 a grey ramp,
unused here), with 4x4 ordered dithering. In 24-bit it is R, G, B bytes.
Frames asked for in order are decoded straight on; going back, or jumping
more than 50 frames ahead, seeks to the key frame before.

Frames that last unevenly (a GIF or APNG holding one picture longer, or a
variable-rate video) are given at one steady rate: the shortest frame's, at
most 50 a second, each picture repeated for as long as it lasts. `frames`
and the rate VOPEN gives count those steady frames, so the Amiga plays
them by its clock as it plays any video.
