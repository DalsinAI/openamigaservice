/*
 * doc.render/1 on the host (docs/DOC_RENDER.md): LibreOffice turns the
 * document into a PDF once (kept in a cache by the file's hash), then
 * poppler's pdfinfo, pdftoppm and pdftotext answer for its pages and text.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#define _GNU_SOURCE
#include "doc_render.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Pages larger than this are refused: 2^26 pixels is 256 MB of ARGB. */
#define MAX_PIXELS (1u << 26)
/* A page's own size, in pixels: 96 per inch. */
#define DPI 96

/* LibreOffice runs one conversion at a time per profile. */
static pthread_mutex_t convert_lock = PTHREAD_MUTEX_INITIALIZER;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static const uint8_t *find(const uint8_t *d, uint32_t n, const void *what, uint32_t len)
{
    return len && n >= len ? memmem(d, n, what, len) : NULL;
}

/* An OLE2 stream name, as UTF-16LE. */
static int has_ole_name(const uint8_t *d, uint32_t n, const char *name)
{
    uint8_t w[64];
    uint32_t i, len = strlen(name);
    for (i = 0; i < len && i < 32; i++) {
        w[2 * i] = (uint8_t)name[i];
        w[2 * i + 1] = 0;
    }
    return find(d, n, w, 2 * len) != NULL;
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

uint32_t dr_sniff(const uint8_t *d, uint32_t n)
{
    if (n >= 5 && !memcmp(d, "%PDF-", 5))
        return FOURCC('P', 'D', 'F', ' ');
    if (n >= 5 && !memcmp(d, "{\\rtf", 5))
        return FOURCC('R', 'T', 'F', ' ');
    if (n >= 4 && !memcmp(d, "\xff" "WPC", 4))
        return FOURCC('W', 'P', 'D', ' ');
    if (n >= 8 && !memcmp(d, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) {
        if (has_ole_name(d, n, "WordDocument"))
            return FOURCC('D', 'O', 'C', ' ');
        if (has_ole_name(d, n, "Workbook") || has_ole_name(d, n, "Book"))
            return FOURCC('X', 'L', 'S', ' ');
        if (has_ole_name(d, n, "PowerPoint Document"))
            return FOURCC('P', 'P', 'T', ' ');
        return 0;
    }
    if (n >= 4 && !memcmp(d, "PK\3\4", 4)) {
        if (n >= 38 && !memcmp(d + 30, "mimetype", 8)) {
            if (find(d, n < 200 ? n : 200, "opendocument.text", 17))
                return FOURCC('O', 'D', 'T', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.spreadsheet", 24))
                return FOURCC('O', 'D', 'S', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.presentation", 25))
                return FOURCC('O', 'D', 'P', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.graphics", 21))
                return FOURCC('O', 'D', 'G', ' ');
            return 0;
        }
        if (find(d, n, "word/", 5))
            return FOURCC('D', 'O', 'C', 'X');
        if (find(d, n, "xl/", 3))
            return FOURCC('X', 'L', 'S', 'X');
        if (find(d, n, "ppt/", 4))
            return FOURCC('P', 'P', 'T', 'X');
    }
    return 0;
}

static const char *extension(uint32_t f)
{
    switch (f) {
    case FOURCC('D', 'O', 'C', 'X'): return "docx";
    case FOURCC('X', 'L', 'S', 'X'): return "xlsx";
    case FOURCC('P', 'P', 'T', 'X'): return "pptx";
    case FOURCC('O', 'D', 'T', ' '): return "odt";
    case FOURCC('O', 'D', 'S', ' '): return "ods";
    case FOURCC('O', 'D', 'P', ' '): return "odp";
    case FOURCC('O', 'D', 'G', ' '): return "odg";
    case FOURCC('D', 'O', 'C', ' '): return "doc";
    case FOURCC('X', 'L', 'S', ' '): return "xls";
    case FOURCC('P', 'P', 'T', ' '): return "ppt";
    case FOURCC('R', 'T', 'F', ' '): return "rtf";
    case FOURCC('W', 'P', 'D', ' '): return "wpd";
    default: return "pdf";
    }
}

/* ---- the cache ---------------------------------------------------------- */

static int cache_dir(char *out, size_t room)
{
    const char *base = getenv("OPENSERVICE_CACHE");
    char tmp[512];

    if (base)
        snprintf(out, room, "%s/doc", base);
    else if ((base = getenv("XDG_CACHE_HOME")) && *base)
        snprintf(out, room, "%s/openservice/doc", base);
    else if ((base = getenv("HOME")) && *base)
        snprintf(out, room, "%s/.cache/openservice/doc", base);
    else
        snprintf(out, room, "/tmp/openservice-doc-%u", (unsigned)getuid());
    /* mkdir -p */
    snprintf(tmp, sizeof tmp, "%s", out);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

/* FNV-1a 64 of the bytes, and their length: the cache key. */
static void key(const uint8_t *d, uint32_t n, char *out, size_t room)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= d[i];
        h *= 1099511628211ULL;
    }
    snprintf(out, room, "%016llx-%u", (unsigned long long)h, (unsigned)n);
}

/* Runs argv with stdout to fd_out (-1: /dev/null); 0 when it exits 0. */
static int run(char *const argv[], int fd_out)
{
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int st = -1, rc;

    posix_spawn_file_actions_init(&fa);
    if (fd_out >= 0)
        posix_spawn_file_actions_adddup2(&fa, fd_out, 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* Runs argv and reads its stdout into a malloc'd, NUL-ended buffer. */
static char *run_read(char *const argv[], size_t *len)
{
    FILE *f = tmpfile();
    char *out = NULL;
    long n;

    if (!f)
        return NULL;
    if (run(argv, fileno(f)) == 0 && (n = ftell(f), fseek(f, 0, SEEK_END), n = ftell(f)) >= 0) {
        rewind(f);
        if ((out = malloc(n + 1)) && fread(out, 1, n, f) == (size_t)n) {
            out[n] = 0;
            if (len)
                *len = n;
        } else {
            free(out);
            out = NULL;
        }
    }
    fclose(f);
    return out;
}

/* The document as a PDF in the cache (path into pdf); 0 on success. */
static int to_pdf(const uint8_t *d, uint32_t n, uint32_t format, char *pdf, size_t room)
{
    char dir[512], k[64], src[700], profile[600];
    struct stat sb;
    FILE *f;
    int rc = -1;

    if (cache_dir(dir, sizeof dir))
        return -1;
    key(d, n, k, sizeof k);
    snprintf(pdf, room, "%s/%s.pdf", dir, k);
    if (stat(pdf, &sb) == 0 && sb.st_size > 0)
        return 0;
    snprintf(src, sizeof src, "%s/%s.%s", dir, k, extension(format));
    if (!(f = fopen(src, "wb")))
        return -1;
    if (fwrite(d, 1, n, f) != n) {
        fclose(f);
        unlink(src);
        return -1;
    }
    fclose(f);
    if (format == FOURCC('P', 'D', 'F', ' '))
        return 0;                               /* src is the PDF */
    snprintf(profile, sizeof profile, "-env:UserInstallation=file://%s/profile", dir);
    pthread_mutex_lock(&convert_lock);
    {
        char *argv[] = { "soffice", profile, "--headless", "--norestore", "--nolockcheck",
                         "--convert-to", "pdf", "--outdir", dir, src, NULL };
        rc = run(argv, -1);
    }
    pthread_mutex_unlock(&convert_lock);
    unlink(src);
    return rc == 0 && stat(pdf, &sb) == 0 && sb.st_size > 0 ? 0 : -1;
}

/* Pages, and the first page's size in points, from pdfinfo. */
static int pdf_info(const char *pdf, uint32_t *pages, double *wpt, double *hpt)
{
    char *argv[] = { "pdfinfo", (char *)pdf, NULL };
    char *out = run_read(argv, NULL), *p;
    int ok = 0;

    if (!out)
        return -1;
    if ((p = strstr(out, "\nPages:")) && sscanf(p + 7, "%u", pages) == 1
        && (p = strstr(out, "\nPage size:")) && sscanf(p + 11, "%lf x %lf", wpt, hpt) == 2
        && *pages > 0 && *wpt > 0 && *hpt > 0)
        ok = 1;
    free(out);
    return ok ? 0 : -1;
}

static void fit(uint32_t w, uint32_t h, uint32_t maxw, uint32_t maxh, uint32_t *ow, uint32_t *oh)
{
    *ow = w;
    *oh = h;
    if (maxw && *ow > maxw) {
        *oh = (uint32_t)((uint64_t)*oh * maxw / *ow);
        *ow = maxw;
    }
    if (maxh && *oh > maxh) {
        *ow = (uint32_t)((uint64_t)*ow * maxh / *oh);
        *oh = maxh;
    }
    if (!*ow)
        *ow = 1;
    if (!*oh)
        *oh = 1;
}

/* Page (0-based) at w x h into argb, through pdftoppm's PPM. */
static int render(const char *pdf, uint32_t page, uint32_t w, uint32_t h, uint8_t *argb)
{
    char first[16], sw[16], sh[16];
    char *argv[] = { "pdftoppm", "-f", first, "-l", first, "-scale-to-x", sw, "-scale-to-y", sh,
                     "-singlefile", "-aa", "yes", "-aaVector", "yes", (char *)pdf, NULL };
    size_t len = 0, at;
    uint32_t pw, ph, maxv, i;
    char *ppm;
    int consumed = 0;

    snprintf(first, sizeof first, "%u", page + 1);
    snprintf(sw, sizeof sw, "%u", w);
    snprintf(sh, sizeof sh, "%u", h);
    if (!(ppm = run_read(argv, &len)))
        return -1;
    if (sscanf(ppm, "P6 %u %u %u%n", &pw, &ph, &maxv, &consumed) != 3 || pw != w || ph != h || maxv != 255) {
        free(ppm);
        return -1;
    }
    at = consumed + 1;                          /* one whitespace byte after maxval */
    if (len < at + (size_t)w * h * 3) {
        free(ppm);
        return -1;
    }
    for (i = 0; i < w * h; i++) {
        argb[4 * i] = 255;
        memcpy(argb + 4 * i + 1, ppm + at + 3 * i, 3);
    }
    free(ppm);
    return 0;
}

/* The text through pdftotext, as ISO-8859-1. */
static char *text(const char *pdf, size_t *len)
{
    char *argv[] = { "pdftotext", "-enc", "Latin1", "-layout", (char *)pdf, "-", NULL };
    return run_read(argv, len);
}

int dr_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct dr_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    char pdf[700];
    uint32_t format, pages, pw, ph, ow, oh;
    double wpt, hpt;

    *result = *aux = 0;
    if (!buf[0].in || !buf[0].length || !(format = dr_sniff(buf[0].in, buf[0].length)))
        return DR_BADREQUEST;
    if (!buf[1].out)
        return DR_TOOSMALL;
    if (op == DR_PROBE && buf[1].length < 24)
        return DR_TOOSMALL;
    if (op != DR_PROBE && op != DR_RENDER && op != DR_TEXT)
        return DR_BADREQUEST;
    if (to_pdf(buf[0].in, buf[0].length, format, pdf, sizeof pdf))
        return DR_BADREQUEST;

    if (op == DR_TEXT) {
        size_t len = 0;
        char *t = text(pdf, &len);
        if (!t)
            return DR_HOSTERROR;
        if (len > buf[1].length) {
            *result = (uint32_t)len;
            free(t);
            return DR_TOOSMALL;
        }
        memcpy(buf[1].out, t, len);
        free(t);
        buf[1].written = (uint32_t)len;
        *result = (uint32_t)len;
        return DR_OK;
    }

    if (pdf_info(pdf, &pages, &wpt, &hpt))
        return DR_BADREQUEST;
    pw = (uint32_t)(wpt * DPI / 72 + 0.5);
    ph = (uint32_t)(hpt * DPI / 72 + 0.5);
    fit(pw, ph, extra[0], extra[1], &ow, &oh);
    if ((uint64_t)ow * oh > MAX_PIXELS)
        return DR_BADREQUEST;

    if (op == DR_PROBE) {
        put32(buf[1].out, DR_KIND_DOCUMENT);
        put32(buf[1].out + 4, format);
        put32(buf[1].out + 8, 0);
        put32(buf[1].out + 12, pages);
        put32(buf[1].out + 16, ow);
        put32(buf[1].out + 20, oh);
        buf[1].written = 24;
        *result = pages;
        return DR_OK;
    }
    if (arg >= pages)
        return DR_BADREQUEST;
    if ((uint64_t)ow * oh * 4 > buf[1].length)
        return DR_TOOSMALL;
    if (render(pdf, arg, ow, oh, buf[1].out))
        return DR_HOSTERROR;
    buf[1].written = ow * oh * 4;
    *result = ow;
    *aux = oh;
    return DR_OK;
}
