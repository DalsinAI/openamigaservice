/*
 * webfetch: openweb.fetch/1 through openservice.device (include/openweb/webfetch.h).
 * No C library beyond memcpy, memset and strlen, so any program can link it.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <proto/exec.h>

#include "openweb/webfetch.h"

static LONG run(struct wf_service *s, UWORD command)
{
    s->io->os_Req.io_Command = command;
    DoIO((struct IORequest *)s->io);
    if (s->io->os_Req.io_Error)
        return s->io->os_Req.io_Error == IOERR_ABORTED ? OSERR_CANCELLED : OSERR_LOST;
    return s->io->os_Status;
}

/* One call of the service, with up to one buffer read (in) and one written (out, buffer 0 or 1). */
static LONG call(struct wf_service *s, UWORD op, ULONG arg, const void *in0, ULONG in0_length,
                 const void *in1, ULONG in1_length, void *out0, ULONG out0_length, const ULONG extra[4])
{
    struct OSRequest *io = s->io;
    if (!s->open)
        return OSERR_NOSERVICE;
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Service = s->handle;
    io->os_Op = op;
    io->os_Arg = arg;
    io->os_Flags = 0;
    if (out0) {
        io->os_Buf[0].ob_Data = out0;
        io->os_Buf[0].ob_Length = out0_length;
        io->os_Flags = 1;
    } else if (in0) {
        io->os_Buf[0].ob_Data = (APTR)in0;
        io->os_Buf[0].ob_Length = in0_length;
    }
    if (in1 && in1_length) {
        io->os_Buf[1].ob_Data = (APTR)in1;
        io->os_Buf[1].ob_Length = in1_length;
    }
    io->os_Extra[0] = extra ? extra[0] : 0;
    io->os_Extra[1] = extra ? extra[1] : 0;
    io->os_Extra[2] = extra ? extra[2] : 0;
    io->os_Extra[3] = extra ? extra[3] : 0;
    return run(s, OSCMD_CALL);
}

LONG wf_service_open(struct wf_service *s)
{
    LONG status;
    memset(s, 0, sizeof *s);
    if (!(s->port = CreateMsgPort()))
        return OSERR_HOST;
    if (!(s->io = (struct OSRequest *)CreateIORequest(s->port, sizeof *s->io))) {
        wf_service_close(s);
        return OSERR_HOST;
    }
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)s->io, 0)) {
        DeleteIORequest((struct IORequest *)s->io);
        s->io = NULL;
        wf_service_close(s);
        return OSERR_NOSERVICE;
    }
    memset(s->io->os_Buf, 0, sizeof s->io->os_Buf);
    s->io->os_Buf[0].ob_Data = (APTR)WF_SERVICE_NAME;
    s->io->os_Buf[0].ob_Length = sizeof WF_SERVICE_NAME - 1;
    if ((status = run(s, OSCMD_OPEN)) != OSERR_OK) {
        wf_service_close(s);
        return status;
    }
    s->handle = (UWORD)s->io->os_Result;
    s->open = 1;
    s->io->os_Service = s->handle;
    s->where = run(s, OSCMD_WHERE) == OSERR_OK ? (UBYTE)s->io->os_Result : 0;
    return OSERR_OK;
}

void wf_service_close(struct wf_service *s)
{
    if (s->io) {
        if (s->open) {
            s->io->os_Service = s->handle;
            run(s, OSCMD_CLOSE);
        }
        if (s->io->os_Req.io_Device)
            CloseDevice((struct IORequest *)s->io);
        DeleteIORequest((struct IORequest *)s->io);
    }
    if (s->port)
        DeleteMsgPort(s->port);
    memset(s, 0, sizeof *s);
}

LONG wf_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG body_length,
             ULONG timeout, const void *body, ULONG part, ULONG *fetch)
{
    ULONG extra[4];
    LONG status;
    extra[0] = flags;
    extra[1] = (flags & WF_BODY) ? body_length : 0;
    extra[2] = timeout;
    extra[3] = 0;
    *fetch = 0;
    status = call(s, WF_OP_OPEN, 0, request, length ? length : strlen(request), (flags & WF_BODY) ? body : NULL,
                  (flags & WF_BODY) ? part : 0, NULL, 0, extra);
    if (status == OSERR_OK)
        *fetch = s->io->os_Result;
    return status;
}

LONG wf_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, BOOL last, ULONG wait, ULONG *taken)
{
    ULONG extra[4];
    LONG status;
    extra[0] = last ? 1 : 0;
    extra[1] = wait;
    extra[2] = extra[3] = 0;
    status = call(s, WF_OP_SEND, fetch, length ? data : NULL, length, NULL, 0, NULL, 0, extra);
    *taken = status == OSERR_OK ? s->io->os_Result : 0;
    return status;
}

static ULONG get32(const UBYTE *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

LONG wf_head(struct wf_service *s, ULONG fetch, UBYTE *buf, ULONG room, ULONG wait, struct wf_head *h)
{
    ULONG extra[4];
    LONG status;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    memset(h, 0, sizeof *h);
    status = call(s, WF_OP_HEAD, fetch, NULL, 0, NULL, 0, buf, room, extra);
    if (status == OSERR_TOOSMALL)
        h->need = s->io->os_Result;
    if (status != OSERR_OK)
        return status;
    if (!s->io->os_Result && !s->io->os_Aux)
        return WF_NOTYET;
    if (room < 32 || get32(buf) != 0x4f574631UL || 32 + get32(buf + 20) + get32(buf + 24) > room)
        return OSERR_BADREQUEST;
    h->error = get32(buf + 4);
    h->status = get32(buf + 8);
    h->flags = get32(buf + 12);
    h->length = get32(buf + 16);
    h->text_length = get32(buf + 20);
    h->info_length = get32(buf + 24);
    h->version = get32(buf + 28);
    h->text = (char *)buf + 32;
    h->info = h->text + h->text_length;
    return OSERR_OK;
}

LONG wf_read(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *end)
{
    ULONG extra[4];
    LONG status;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    status = call(s, WF_OP_READ, fetch, NULL, 0, NULL, 0, buf, room, extra);
    *got = status == OSERR_OK ? s->io->os_Result : 0;
    *end = status == OSERR_OK ? s->io->os_Aux : 1;
    return status;
}

LONG wf_cancel(struct wf_service *s, ULONG fetch)
{
    return call(s, WF_OP_CANCEL, fetch, NULL, 0, NULL, 0, NULL, 0, NULL);
}

LONG wf_close(struct wf_service *s, ULONG fetch)
{
    return call(s, WF_OP_CLOSE, fetch, NULL, 0, NULL, 0, NULL, 0, NULL);
}

static ULONG put(char *out, ULONG at, ULONG room, const char *s)
{
    ULONG n = strlen(s);
    if (at + n >= room)
        return room;                            /* does not fit */
    memcpy(out + at, s, n);
    return at + n;
}

ULONG wf_request(char *out, ULONG room, const char *method, const char *url, const char *const *headers, LONG count)
{
    ULONG at = 0;
    LONG i;
    at = put(out, at, room, method);
    at = put(out, at, room, " ");
    at = put(out, at, room, url);
    at = put(out, at, room, "\n");
    for (i = 0; i < count && at < room; i++) {
        at = put(out, at, room, headers[i]);
        at = put(out, at, room, "\n");
    }
    if (at >= room)
        return 0;
    out[at] = 0;
    return at;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

BOOL wf_header(const struct wf_head *h, const char *name, char *out, ULONG room)
{
    const char *p = h->text, *end = h->text + h->text_length;
    ULONG n = strlen(name);
    if (!p || !room)
        return FALSE;
    while (p < end) {
        const char *nl = p;
        ULONG i;
        while (nl < end && *nl != '\n')
            nl++;
        for (i = 0; i < n && p + i < nl && lower(p[i]) == lower(name[i]); i++)
            ;
        if (i == n && p + n < nl && p[n] == ':') {
            const char *v = p + n + 1;
            ULONG k = 0;
            while (v < nl && (*v == ' ' || *v == '\t'))
                v++;
            while (v < nl && k + 1 < room)
                out[k++] = *v++;
            out[k] = 0;
            return TRUE;
        }
        p = nl + 1;
    }
    return FALSE;
}

const char *wf_error_name(ULONG error)
{
    static const char *const names[] = { "none", "DNS", "connect", "TLS", "timeout", "protocol",
                                         "cancelled", "closed early", "decode", "no room on the cores" };
    return error < sizeof names / sizeof names[0] ? names[error] : "unknown";
}
