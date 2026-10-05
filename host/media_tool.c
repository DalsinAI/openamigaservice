/*
 * media.decode/1, the half that runs host tools (media_tool.h).
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#define _GNU_SOURCE
#include "media_tool.h"
#include "media_decode.h"
#include "hostrun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Larger pictures are refused: 2^28 pixels is 1 GB of ARGB. */
#define MAX_PIXELS (1u << 28)

void md_hint(uint32_t hint, char out[8])
{
    int i, n = 0;

    for (i = 0; i < 4; i++) {
        char c = (char)(hint >> (24 - 8 * i));
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out[n++] = c;
        else
            break;
    }
    out[n] = 0;
}

int md_is_raw(const uint8_t *d, uint32_t n, const char *hint)
{
    static const char *const raw[] = { "cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "dng", "orf",
                                       "rw2", "raf", "pef", "srw", "x3f", "erf", "kdc", "dcr", "mrw", "3fr",
                                       "iiq", "rwl", "mos", "raw", NULL };
    const char *const *r;
    uint32_t i;

    for (r = raw; *r; r++)
        if (!strcmp(hint, *r))
            return 1;
    /* A TIFF with a DNGVersion tag (0xC612) in its first directory. */
    if (n >= 16 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4))) {
        int le = d[0] == 'I';
        uint32_t ifd = le ? d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24
                          : (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
        uint32_t count;
        if (ifd + 2 > n)
            return 0;
        count = le ? d[ifd] | d[ifd + 1] << 8 : d[ifd] << 8 | d[ifd + 1];
        for (i = 0; i < count && ifd + 2 + i * 12 + 2 <= n; i++) {
            const uint8_t *e = d + ifd + 2 + i * 12;
            if ((le ? e[0] | e[1] << 8 : e[0] << 8 | e[1]) == 0xC612)
                return 1;
        }
    }
    /* Canon CR3: ISO media with the crx brand. */
    return n >= 12 && !memcmp(d + 4, "ftypcrx ", 8);
}

/* ---- PNM and PAM --------------------------------------------------------- */

static int token(const uint8_t *d, size_t n, size_t *pos, char *out, size_t room)
{
    size_t k = 0;

    for (;;) {
        while (*pos < n && (d[*pos] == ' ' || d[*pos] == '\t' || d[*pos] == '\r' || d[*pos] == '\n'))
            (*pos)++;
        if (*pos < n && d[*pos] == '#') {
            while (*pos < n && d[*pos] != '\n')
                (*pos)++;
            continue;
        }
        break;
    }
    while (*pos < n && k + 1 < room && d[*pos] > ' ')
        out[k++] = (char)d[(*pos)++];
    out[k] = 0;
    return k > 0;
}

/* P5/P6 (8-bit) and P7 (PAM, 1 to 4 channels, 8-bit) to ARGB. */
static int parse_pnm(const uint8_t *d, size_t n, uint32_t *flags, uint32_t *width, uint32_t *height,
                     uint8_t **argb)
{
    char t[32];
    size_t pos = 2, px, i;
    uint32_t w = 0, h = 0, depth = 0, maxval = 0;
    const uint8_t *s;
    uint8_t *o;

    if (n < 3 || d[0] != 'P' || (d[1] != '5' && d[1] != '6' && d[1] != '7'))
        return MD_HOSTERROR;
    if (d[1] == '7') {
        while (token(d, n, &pos, t, sizeof t) && strcmp(t, "ENDHDR")) {
            char v[32];
            if (!strcmp(t, "TUPLTYPE")) {
                token(d, n, &pos, v, sizeof v);
                continue;
            }
            if (!token(d, n, &pos, v, sizeof v))
                return MD_HOSTERROR;
            if (!strcmp(t, "WIDTH"))
                w = strtoul(v, NULL, 10);
            else if (!strcmp(t, "HEIGHT"))
                h = strtoul(v, NULL, 10);
            else if (!strcmp(t, "DEPTH"))
                depth = strtoul(v, NULL, 10);
            else if (!strcmp(t, "MAXVAL"))
                maxval = strtoul(v, NULL, 10);
        }
    } else {
        depth = d[1] == '5' ? 1 : 3;
        if (!token(d, n, &pos, t, sizeof t) || !(w = strtoul(t, NULL, 10))
            || !token(d, n, &pos, t, sizeof t) || !(h = strtoul(t, NULL, 10))
            || !token(d, n, &pos, t, sizeof t))
            return MD_HOSTERROR;
        maxval = strtoul(t, NULL, 10);
    }
    pos++;                                      /* the one whitespace before the pixels */
    if (!w || !h || depth < 1 || depth > 4 || maxval != 255 || (uint64_t)w * h > MAX_PIXELS)
        return MD_HOSTERROR;
    px = (size_t)w * h;
    if (pos > n || n - pos < px * depth)
        return MD_HOSTERROR;
    if (!argb) {
        *width = w;
        *height = h;
        *flags = depth == 2 || depth == 4 ? MD_FLAG_ALPHA : 0;
        return MD_OK;
    }
    if (!(o = *argb = malloc(px * 4)))
        return MD_HOSTERROR;
    for (i = 0, s = d + pos; i < px; i++, s += depth, o += 4)
        switch (depth) {
        case 1: o[0] = 255;  o[1] = o[2] = o[3] = s[0]; break;
        case 2: o[0] = s[1]; o[1] = o[2] = o[3] = s[0]; break;
        case 3: o[0] = 255;  o[1] = s[0]; o[2] = s[1]; o[3] = s[2]; break;
        default: o[0] = s[3]; o[1] = s[0]; o[2] = s[1]; o[3] = s[2]; break;
        }
    *width = w;
    *height = h;
    *flags = depth == 2 || depth == 4 ? MD_FLAG_ALPHA : 0;
    return MD_OK;
}

/* ---- pictures ------------------------------------------------------------ */

/* The file converted to PNM/PAM in the cache (path into out); 0 on success. */
static int convert(const uint8_t *d, uint32_t n, const char *hint, int raw, char *out, size_t room)
{
    char dir[512], k[64], tmp[512], src[600], dst[600], arg[640];
    struct stat sb;
    int rc = -1;

    if (hr_cache_dir("picture", dir, sizeof dir))
        return -1;
    hr_key(d, n, k, sizeof k);
    snprintf(out, room, "%s/%s.pam", dir, k);
    if (stat(out, &sb) == 0 && sb.st_size > 0)
        return 0;
    if (hr_tempdir(tmp, sizeof tmp))
        return -1;
    snprintf(src, sizeof src, "%s/in.%s", tmp, *hint ? hint : "img");
    snprintf(dst, sizeof dst, "%s/out.pam", tmp);
    if (hr_write(src, d, n) == 0) {
        if (raw) {
            /* LibRaw, camera white balance, 8-bit PPM; dcraw when LibRaw's tools are missing. */
            char *emu[] = { "dcraw_emu", "-w", "-Z", dst, src, NULL };
            if ((rc = hr_run(emu, -1)) != 0) {
                FILE *f = fopen(dst, "wb");
                char *dc[] = { "dcraw", "-c", "-w", src, NULL };
                if (f) {
                    rc = hr_run(dc, fileno(f));
                    fclose(f);
                }
            }
        } else {
            char *im[] = { "convert", arg, "-depth", "8", dst, NULL };
            snprintf(arg, sizeof arg, "%s[0]", src);
            rc = hr_run(im, -1);
        }
        if (rc == 0 && (stat(dst, &sb) != 0 || sb.st_size == 0 || rename(dst, out) != 0))
            rc = -1;
    }
    hr_rmdir(tmp);
    return rc;
}

int md_tool_picture(const uint8_t *d, uint32_t n, const char *hint, int raw, uint32_t *format,
                    uint32_t *flags, uint32_t *width, uint32_t *height, uint8_t **argb)
{
    char path[600];
    uint8_t *pnm;
    size_t len;
    int st, i;

    if (argb)
        *argb = NULL;
    if (convert(d, n, hint, raw, path, sizeof path))
        return MD_BADREQUEST;
    if (!(pnm = hr_read(path, &len)))
        return MD_HOSTERROR;
    st = parse_pnm(pnm, len, flags, width, height, argb);
    free(pnm);
    if (raw)
        *format = MD_FORMAT_RAW;
    else if (*hint) {
        size_t hl = strlen(hint);
        *format = 0;
        for (i = 0; i < 4; i++) {          /* the hint in capitals, space-padded: 'TGA ' */
            char c = (size_t)i < hl ? hint[i] : ' ';
            *format = *format << 8 | (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    } else
        *format = MD_FORMAT_MAGICK;
    return st;
}

/* ---- tunes --------------------------------------------------------------- */

uint32_t md_tune_sniff(const uint8_t *d, uint32_t n)
{
    /* A standard MIDI file: a six-byte header, format 0 to 2, a track, then MTrk. */
    if (n >= 22 && !memcmp(d, "MThd", 4) && !memcmp(d + 4, "\0\0\0\6", 4) && d[8] == 0 && d[9] <= 2
        && (d[10] | d[11]) && !memcmp(d + 14, "MTrk", 4))
        return MD_FORMAT_MIDI;
    if (n >= 20 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "RMID", 4))
        return MD_FORMAT_MIDI;
    if (n >= 0x76 && (!memcmp(d, "PSID", 4) || !memcmp(d, "RSID", 4)))
        return MD_FORMAT_SID;
    return 0;
}

static const char *soundfont(void)
{
    static const char *const fonts[] = { "/usr/share/sounds/sf2/FluidR3_GM.sf2", "/usr/share/sounds/sf2/default-GM.sf2",
                                         "/usr/share/soundfonts/default.sf2", "/usr/share/soundfonts/FluidR3_GM.sf2",
                                         NULL };
    const char *env = getenv("OPENSERVICE_SOUNDFONT");
    const char *const *f;

    if (env && *env)
        return env;
    for (f = fonts; *f; f++)
        if (access(*f, R_OK) == 0)
            return *f;
    return NULL;
}

uint8_t *md_tune_render(const uint8_t *d, uint32_t n, uint32_t kind, uint32_t *len)
{
    char dir[512], k[64], tmp[512], src[600], dst[600], wav[600], opt[640], secs[32];
    const char *env;
    struct stat sb;
    uint8_t *out = NULL;
    size_t got;
    int rc = -1;

    if (hr_cache_dir("sound", dir, sizeof dir))
        return NULL;
    hr_key(d, n, k, sizeof k);
    snprintf(wav, sizeof wav, "%s/%s.wav", dir, k);
    if (!(stat(wav, &sb) == 0 && sb.st_size > 44)) {
        if (hr_tempdir(tmp, sizeof tmp))
            return NULL;
        snprintf(src, sizeof src, "%s/in.%s", tmp, kind == MD_FORMAT_MIDI ? "mid" : "sid");
        snprintf(dst, sizeof dst, "%s/out.wav", tmp);
        if (hr_write(src, d, n) == 0) {
            if (kind == MD_FORMAT_MIDI) {
                const char *sf = soundfont();
                char *fs[] = { "fluidsynth", "-n", "-i", "-q", "-F", dst, "-r", "44100", "-T", "wav",
                               (char *)sf, src, NULL };
                if (sf)
                    rc = hr_run(fs, -1);
            } else {
                /* SID tunes play for ever; OPENSERVICE_SIDSECONDS of it (default 180). */
                env = getenv("OPENSERVICE_SIDSECONDS");
                snprintf(secs, sizeof secs, "-t%u", env && atoi(env) > 0 ? (unsigned)atoi(env) : 180u);
                snprintf(opt, sizeof opt, "-w%s", dst);
                {
                    char *sp[] = { "sidplayfp", "-q", secs, opt, src, NULL };
                    rc = hr_run(sp, -1);
                }
            }
            if (rc == 0 && (stat(dst, &sb) != 0 || sb.st_size <= 44 || rename(dst, wav) != 0))
                rc = -1;
        }
        hr_rmdir(tmp);
        if (rc)
            return NULL;
    }
    if ((out = hr_read(wav, &got)))
        *len = (uint32_t)got;
    return out;
}
