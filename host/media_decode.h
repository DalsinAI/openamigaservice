/*
 * media.decode/1: pictures for an Amiga's datatypes, decoded on the host
 * (docs/MEDIA_DECODE.md). One call per request; no state between calls.
 * The emulator's service host and the LAN Cradle both call this.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#ifndef MEDIA_DECODE_H
#define MEDIA_DECODE_H

#include <stdint.h>

#define MD_PROBE  1   /* buf0 file, extra[0..1] largest size -> buf1 info (24 bytes); result width, aux height */
#define MD_DECODE 2   /* arg frame, buf0 file, extra[0..1] largest size -> buf1 ARGB; result width, aux height */

#define MD_KIND_PICTURE 1
#define MD_KIND_SOUND   3

#define MD_FORMAT_AVIF 0x41564946u   /* 'AVIF' */
#define MD_FORMAT_HEIC 0x48454943u   /* 'HEIC' */
#define MD_FORMAT_FLAC   0x464c4143u /* 'FLAC' */
#define MD_FORMAT_VORBIS 0x564f5242u /* 'VORB' */
#define MD_FORMAT_OPUS   0x4f505553u /* 'OPUS' */
#define MD_FORMAT_MP3    0x4d503320u /* 'MP3 ' */
#define MD_FORMAT_AAC    0x41414320u /* 'AAC ' */
#define MD_FORMAT_ALAC   0x414c4143u /* 'ALAC' */
#define MD_FORMAT_WMA    0x574d4120u /* 'WMA ' */
#define MD_FORMAT_SOUND  0x534f554eu /* 'SOUN': another sound FFmpeg reads */

#define MD_FLAG_ALPHA 1

#define MD_INFO_SIZE 24

/* Status, as the services card's. */
#define MD_OK          0
#define MD_BADREQUEST (-2)
#define MD_TOOSMALL   (-4)
#define MD_HOSTERROR  (-5)

struct md_buffer {
    const uint8_t *in;     /* the Amiga's bytes, for a buffer the service reads */
    uint8_t *out;          /* where to write, for a buffer the service writes */
    uint32_t length;       /* in: its length; out: the room */
    uint32_t written;      /* out: bytes written */
};

/* Runs one request. Returns the status; sets *result and *aux. */
int md_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux);

/* Big-endian u32 at p. */
void md_put32(uint8_t *p, uint32_t v);

/* The size a w x h picture is written at, to fit inside maxw x maxh (0: any). */
void md_fit(uint32_t w, uint32_t h, uint32_t maxw, uint32_t maxh, uint32_t *ow, uint32_t *oh);

#endif
