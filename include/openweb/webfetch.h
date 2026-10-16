/*
 * webfetch: the Amiga's side of openweb.fetch/1 (docs/OPENWEB_FETCH.md), for
 * any program to link: OWFetch now, OpenBrowser later.
 *
 *   struct wf_service s;
 *   wf_service_open(&s);                     -> 0, or OSERR_NOSERVICE
 *   wf_open(&s, "GET https://example.com/\n", 0, WF_DECODE, 0, 0, NULL, 0, &fetch);
 *   wf_head(&s, fetch, buf, room, 500, &head) until it returns 0
 *   wf_read(&s, fetch, buf, room, 1000, &got, &end) until end
 *   wf_close(&s, fetch);
 *   wf_service_close(&s);
 *
 * Each call is one request through openservice.device, which takes it to
 * the services card or a paired Cradle. A struct wf_service has one
 * IORequest, so its calls run one at a time; fetches running side by side
 * each want their own wf_service (or their own task). The waits are in
 * milliseconds, at most 2000: a call answers sooner when something happens.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#ifndef OPENWEB_WEBFETCH_H
#define OPENWEB_WEBFETCH_H

#include <exec/types.h>
#include "devices/openservice.h"

#define WF_SERVICE_NAME "openweb.fetch/1"

/* Ops */
#define WF_OP_OPEN   1
#define WF_OP_SEND   2
#define WF_OP_HEAD   3
#define WF_OP_READ   4
#define WF_OP_CANCEL 5
#define WF_OP_CLOSE  6
#define WF_OP_STATS  7

/* wf_open flags */
#define WF_BODY   1UL           /* a body follows: body, then wf_send */
#define WF_DECODE 2UL           /* gzip and deflate undone on the cores */
#define WF_LENGTH_UNKNOWN 0xffffffffUL

/* wf_head: the head's flags */
#define WF_H_TLS      1UL
#define WF_H_VERIFIED 2UL
#define WF_H_DECODED  4UL
#define WF_H_LENGTH   8UL
#define WF_H_NOBODY   16UL

/* Errors: struct wf_head's error, and wf_read's *end >> 8 */
#define WF_E_DNS       1
#define WF_E_CONNECT   2
#define WF_E_TLS       3
#define WF_E_TIMEOUT   4
#define WF_E_PROTOCOL  5
#define WF_E_CANCELLED 6
#define WF_E_CLOSED    7
#define WF_E_DECODE    8
#define WF_E_HOST      9

/* wf_head's answer when the head has not come yet. */
#define WF_NOTYET 1

struct wf_service {
    struct MsgPort *port;
    struct OSRequest *io;
    UWORD handle;               /* the service's, from OSCMD_OPEN */
    UBYTE open;
    UBYTE where;                /* OSWHERE_CARD or OSWHERE_LAN */
};

struct wf_head {
    ULONG error;                /* 0, or WF_E_*: then status is 0 */
    ULONG status;               /* the HTTP status */
    ULONG flags;                /* WF_H_* */
    ULONG length;               /* the body's length, with WF_H_LENGTH */
    ULONG version;              /* 10 or 11 */
    char *text;                 /* in the caller's buffer: the status line and the headers, each line ending in LF */
    ULONG text_length;
    char *info;                 /* key=value lines: the TLS summary, or error= */
    ULONG info_length;
    ULONG need;                 /* with OSERR_TOOSMALL: the room the head needs */
};

/* Opens openweb.fetch/1: 0, or OSERR_NOSERVICE (or another OSERR_*). */
LONG wf_service_open(struct wf_service *s);
void wf_service_close(struct wf_service *s);

/* Starts a fetch. request: "METHOD URL\n" then "Name: value\n" lines
 * (wf_request makes one); length 0 means NUL ended. With WF_BODY, body_length
 * is the body's whole length (or WF_LENGTH_UNKNOWN) and body/part its first
 * part (may be 0). timeout in seconds, 0 for 30. 0 and *fetch, or OSERR_*. */
LONG wf_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG body_length,
             ULONG timeout, const void *body, ULONG part, ULONG *fetch);
/* The next part of the body (last: the final part). *taken is 0 when the
 * queue was full: send the same part again. */
LONG wf_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, BOOL last, ULONG wait, ULONG *taken);
/* The head into buf, parsed into *h: 0 when it is there (or the fetch
 * failed: h->error), WF_NOTYET, OSERR_TOOSMALL (h->need), or OSERR_*. */
LONG wf_head(struct wf_service *s, ULONG fetch, UBYTE *buf, ULONG room, ULONG wait, struct wf_head *h);
/* Body bytes into buf: *got of them; *end 0 while more is to come, else 1 |
 * the error << 8 (0 when the body ended well). */
LONG wf_read(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *end);
LONG wf_cancel(struct wf_service *s, ULONG fetch);
LONG wf_close(struct wf_service *s, ULONG fetch);

/* The request text from its parts, NUL ended, into out: its length, or 0
 * when it does not fit. headers: count strings "Name: value". */
ULONG wf_request(char *out, ULONG room, const char *method, const char *url, const char *const *headers, LONG count);
/* A header's value from the head's text (the name in any case), NUL ended
 * into out; FALSE when the head has none. */
BOOL wf_header(const struct wf_head *h, const char *name, char *out, ULONG room);
/* An error's name, "DNS", "TLS", ... */
const char *wf_error_name(ULONG error);

#endif
