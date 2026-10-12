# openweb.fetch/1

HTTP and HTTPS fetches for an Amiga: the network and TLS run on the x86 or
ARM64 cores, and the Amiga gets the status, the headers and the body. It is
the first bridge service for the browser (AmigaChrome's
`MAGIC_FUNCTIONS_AND_BRIDGE.md`, section 12), and any other program may use
it. Host code: `host/openweb_fetch.c`, built into `host/libopenwebfetch.so`
(entry `wf_call`), with OpenSSL 3 and zlib. On the Amiga: the client
`src/webfetch.c` (`include/openweb/webfetch.h`) and the `OWFetch` command
(`src/owfetch.c`).

Status: v1, 12 October 2026. Offered on the services card. Not yet on the
LAN (below).

## The shape

A fetch is a handle on the host side. OPEN starts it and returns at once;
the connection, the TLS handshake and the request run on a thread of its
own. The Amiga then asks for the head, and reads the body in chunks into
its own buffers, while the host reads ahead into a window. A body to upload
goes with OPEN, or in chunks with SEND. CANCEL stops one fetch; CLOSE ends
it.

The card's completions are the rings, and openservice.device turns each
into a reply on the caller's port, so a program waits on a signal bit and
can have many fetches in flight, each with its own handle.

## Ops

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | OPEN | buf0 the request text (below); buf1 the body or its first part (with `WF_BODY`); `extra[0]` flags; `extra[1]` the body's whole length, `$FFFFFFFF` when not known; `extra[2]` the timeout in seconds (0: 30) | result the fetch's handle |
| 2 | SEND | `arg` the handle; buf0 the next part of the body; `extra[0]` bit 0: the last part; `extra[1]` the wait in ms | result the bytes taken: all of buf0, or 0 when the host's queue is full (send it again) |
| 3 | HEAD | `arg` the handle; buf0 (out) room for the head (below); `extra[0]` the wait in ms | result the HTTP status, aux the error (below). Result 0 and aux 0: the head has not come yet, ask again |
| 4 | READ | `arg` the handle; buf0 (out) room for body bytes; `extra[0]` the wait in ms | result the bytes written; aux 0 more to come, 1 the end; on a failure, 1 with the error in bits 8-15 |
| 5 | CANCEL | `arg` the handle | 0. The fetch stops now: its connection is shut, and a HEAD or READ waiting on it answers with error 6 |
| 6 | CLOSE | `arg` the handle | 0. Cancels it if it still runs; the handle is gone |
| 7 | STATS | | result the fetches open, aux their threads still running |

OPEN flags (`extra[0]`):

- bit 0 `WF_BODY`: the request has a body. Its bytes are buf1, then each
  SEND, up to `extra[1]` bytes when that is known (then sent with
  `Content-Length`), or up to a SEND marked last (then sent chunked).
- bit 1 `WF_DECODE`: the host asks for `gzip, deflate` and undoes them.
  It drops `Content-Encoding` and the sender's `Content-Length`; HEAD gives
  the length after decoding only when the whole body is in by then.

A wait is how long the call may wait for something to happen before it
answers: 0 answers at once, and more than 2000 is cut to 2000.

### The request text

ISO-8859-1 or UTF-8, lines ending in LF (a CR before it is dropped):

```
GET https://example.com/path?q=1
Cookie: a=1; b=2
Accept: text/html
```

The first line is the method and the absolute URL (`http://` or
`https://`, with an optional port; an IPv6 address in brackets). Then one
header per line, as it should be sent. The host writes `Host`,
`Connection`, `Content-Length` or `Transfer-Encoding`, and with
`WF_DECODE` `Accept-Encoding`; the Amiga's own copies of those are dropped.
It adds `User-Agent: OpenWeb-Fetch/1` when there is none. Everything else
goes as it is: the Amiga keeps its own cookie jar and sends `Cookie`
itself.

### The head

A 32-byte block of big-endian u32s, then two texts:

| Offset | Field |
| --- | --- |
| +0 | `'OWF1'` |
| +4 | error: 0, or why the fetch failed (below) |
| +8 | the HTTP status (0 on an error) |
| +12 | flags: bit 0 TLS, bit 1 the certificate was checked and trusted, bit 2 the body was decoded, bit 3 the length is known, bit 4 there is no body (HEAD, 204, 304) |
| +16 | the body's length, when bit 3 |
| +20 | the bytes of header text that follow |
| +24 | the bytes of info text after the header text |
| +28 | the HTTP version times ten (10 or 11) |

The header text is the status line, then each header as the server sent it
(`Name: value`), each line ending in LF, in order, repeats kept: every
`Set-Cookie` comes back to the Amiga, and the host keeps none of them.
`Transfer-Encoding` is dropped, because the body comes de-chunked, and so
are `Content-Encoding` and `Content-Length` when the body was decoded.

The info text is `key=value` lines. After a TLS handshake:

```
tls=TLSv1.3
cipher=TLS_AES_256_GCM_SHA384
subject=CN=example.com
issuer=CN=DigiCert Global G3 TLS ECC SHA384 2020 CA1,O=DigiCert Inc,C=US
expires=Jan 15 23:59:59 2027 GMT
verified=yes
```

On an error, `error=` says what went wrong, and for a certificate the
host does not trust, the summary of the certificate the server sent:

```
error=self-signed certificate
subject=CN=localhost
issuer=CN=localhost
expires=Oct 13 10:00:00 2026 GMT
```

When buf0 is too small, HEAD answers -4 with the bytes it needs in result;
the head stays, so ask again with more room.

### Errors

Errors are apart from HTTP statuses: a 404 is a response, with error 0.

| Error | Name | Meaning |
| --- | --- | --- |
| 1 | DNS | The name did not resolve |
| 2 | CONNECT | No connection: refused, or no route |
| 3 | TLS | The handshake failed, or the certificate is not trusted (info says why) |
| 4 | TIMEOUT | No answer within the timeout |
| 5 | PROTOCOL | The answer was not HTTP/1.x, or its head was over 64 KB |
| 6 | CANCELLED | CANCEL or CLOSE stopped it |
| 7 | CLOSED | The connection closed before the body ended, or during the upload |
| 8 | DECODE | The gzip or deflate data was bad |
| 9 | HOST | No memory or no thread for it |

Statuses: 0; -2 for a request the service cannot use (a bad request text,
an unknown handle or op); -4 above; -5 when 64 fetches are open already.

## What it does, against section 12

- **Request:** method, absolute URL and headers in OPEN; the body in OPEN
  and SEND, streamed.
- **Response:** status and headers in HEAD; the body in READ, chunk by
  chunk into the Amiga's buffers. `Set-Cookie` comes back, and is not kept.
- **Errors apart:** HEAD's error field, with the words in the info text;
  TLS says what was wrong with the certificate.
- **Redirects** are never followed: a 3xx comes back with its `Location`,
  for the caller to follow.
- **gzip and deflate** are undone when the Amiga asks (`WF_DECODE`), with
  `Content-Encoding` dropped and the length fixed as above.
- **Many at once:** each fetch has its own handle and thread; completions
  come through the card's rings, so the Amiga waits on a signal bit.
  CANCEL ends one fetch.
- **TLS** is checked against the trust store of the system the service
  runs on (OpenSSL's default paths), with the name checked against the
  certificate, TLS 1.2 or later; a short summary of the certificate comes
  back in every HTTPS head.

## Limits

- 64 fetches open at once.
- The request text up to 64 KB; a response head up to 64 KB.
- The host reads at most 1 MB ahead of the Amiga for each fetch, and stops
  reading the connection while that is full; it queues at most 1 MB of the
  upload.
- A wait is at most 2 s. The timeout (default 30 s, at most 300 s) covers
  connecting, the handshake, the head and each pause in the body. A name
  that takes long to resolve is not cut short by it.
- The card's buffer rules apply: at most 16 MB written per request.
- The card has four workers for all its services, and a HEAD or READ that
  waits holds one for its wait. Keep waits short (250 to 1000 ms) and one
  waiting call per fetch.
- A card CANCEL (AbortIO) answers a waiting call at once, but the wait
  runs out on the host side; CANCEL or CLOSE the fetch to stop its
  connection too.

## Why C and OpenSSL

The library runs inside the emulator's process, on the card's workers. A
fetch needs a thread that can block on its connection and be stopped from
another thread. In C each fetch owns its socket, and CANCEL shuts it, which
ends any read at once. OpenSSL 3 and zlib are already on every system that
builds `libopentlskey.so` and `libmediadecode.so`, and their system trust
store is the one the rest of the system uses. A Python helper would be one
more process to start, watch and stop from inside the emulator.

## The client on the Amiga

`src/webfetch.c` holds the calls, so any program can link it: open the
service, start a fetch, read its head (parsed into status, flags, headers
and info), read and send body parts, cancel, close. Each call is one
request through openservice.device. `OWFetch` is the command built on it:

```
OWFetch URL [TO file] [HEAD] [HEADER "Name: value" ...] [METHOD m] [FROM file]
        [TIMEOUT s] [RAW] [QUIET] [INFO]
```

It prints the status line and the headers (the TLS summary too with
`INFO`), and writes the body to `TO`, or to the console. `HEAD` asks for
the head only, `FROM` sends a file as the body (POST unless `METHOD` says
otherwise), `RAW` keeps the body as the server sent it, and Ctrl-C cancels.

## Left for later

- **Connections kept.** v1 opens a connection for each fetch and closes it
  (`Connection: close`). Keeping them per host, and HTTP/2 sessions with
  their streams, is the next step for page loads: the handles and ops stay
  as they are.
- **Brotli** (`br`): no library for it on the build systems yet.
- **The card completing later.** A library call now runs to its end on a
  card worker. A way for a library to answer later, from its own thread,
  would let a waiting READ hold no worker. That is a change in the card's
  host (`services.c`), not in this service's messages.
- **The LAN transport for real Amigas.** The device already carries the
  same entries to a paired Cradle. Two things are needed first: the
  Cradle's host (`cradle/nursery_host.py`) offering this library, and the
  device keeping every call on a fetch's handle to the one Cradle that
  opened it (today a service open on several Cradles spreads calls to the
  least busy one, which suits stateless services only). A long wait also
  holds the Cradle's connection, so the LAN path wants short waits.
- **Proxies, client certificates, and a trust store of the Amiga's own.**
- **The PiStorm back end** (below).

### The PiStorm back end (design only)

The appliance's PiStorm plan (amigachrome `appliance/pistorm/README.md`)
runs Linux on the Pi with the AmigaChrome runtime on its ARM64 cores, and
the A1200's own bus for its keyboard, mouse and floppy. There the services
card is the same card in the same runtime, so this service needs no change:

- `libopenwebfetch.so` built for ARM64 from the same source; it needs only
  the C library, OpenSSL 3 and zlib, which Pi OS has.
- The Pi's own Ethernet or Wi-Fi carries the traffic. The Amiga needs no
  network card and no TCP/IP stack for it.
- The trust store is Pi OS's `ca-certificates`. The Pi has no clock that
  keeps time with the power off, so certificates fail as "not yet valid"
  until the clock is set: the appliance should wait for NTP before it
  offers the service, or HEAD will report TLS errors that are not the
  server's fault.
- Memory: 64 fetches with 1 MB windows is 64 MB at most; a Pi with 1 or
  2 GB may want a smaller window or fewer fetches, as a build setting.
- With Emu68 instead of the appliance, there is no Linux on the Pi, so
  no sockets or TLS beside the 68k. Such an Amiga reaches the service on a
  Cradle over the LAN, which waits on the LAN transport above.

## Tests

`host/test_openweb_fetch.py` runs the library against servers it starts
on 127.0.0.1: plain HTTP (headers, `Set-Cookie`, a redirect not followed,
chunked, close-delimited, gzip with and without `WF_DECODE`, several MB
checked by MD5, uploads with a length and chunked, HEAD), errors (DNS,
connection refused, timeout, cancel, closed early), HTTPS with a test
certificate the test makes with `openssl` in a temporary directory (the
TLS error and its summary; the same server trusted through
`SSL_CERT_FILE` in a child process, and a name mismatch), eight fetches at
once, the limit of 64, and https://example.com when the network allows.
