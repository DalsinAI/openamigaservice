/*
 * media.decode/1, the FFmpeg half (media_av.c): sound now, video later.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#ifndef MEDIA_AV_H
#define MEDIA_AV_H

#include <stdint.h>
#include "media_decode.h"

/* 1 when FFmpeg recognises the file. */
int md_is_av(const uint8_t *d, uint32_t n);
/* PROBE and DECODE for a sound: extra[0] most channels, extra[1] highest rate. */
int md_sound_probe(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                   uint32_t *result, uint32_t *aux);
int md_sound_decode(const struct md_buffer *file, uint32_t first, const uint32_t extra[4], struct md_buffer *out,
                    uint32_t *result, uint32_t *aux);

#endif
