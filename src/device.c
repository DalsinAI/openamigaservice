/*
 * openservice.device (devices/openservice.h).
 *
 * One worker process does everything: it owns the services card's rings and
 * the LAN connections, so nothing here needs a lock beyond Forbid() around
 * the device's own open count. BeginIO hands each request to the worker's
 * port; the worker writes it to the card's submission ring, or sends it to
 * a Cradle, and replies when the completion comes back.
 *
 * The card's wire is design/SERVICES_CARD.md (v1). Over the LAN each frame
 * is the same 64-byte entry, then the buffers the service reads; the answer
 * is the 16-byte completion, then for each buffer the service writes a u32
 * byte count and that many bytes.
 *
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include <string.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#include "devices/openservice.h"
#include "openservice.h"
#include "mdns.h"

#ifndef REG
#define REG(reg, arg) arg __asm(#reg)
#endif

const char DevName[] = OPENSERVICE_NAME;
/* "Version DEVS:openservice.device" reads this; the ROMTag shows the rest. */
const char DevVersion[] __attribute__((used)) = "$VER: openservice.device 1.0 (5.10.2026)";
#define DevIdString (DevVersion + 6)
#define DEV_VERSION 1
#define DEV_REVISION 0

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct ExpansionBase *ExpansionBase;
struct Library *SocketBase;          /* the worker's own */

static BPTR segList;
static struct Process *worker;
static struct MsgPort *workerPort;   /* the worker's: requests arrive here */
static BYTE cardSignal = -1, abortSignal = -1, quitSignal = -1;
static struct Task *starter;         /* waiting for the worker to start or stop */

/* ---- the card ------------------------------------------------------------- */

#define REG_MAGIC    0x00
#define REG_VERSION  0x04
#define REG_RINGS    0x08
#define REG_ORDER    0x0c
#define REG_DOORBELL 0x10
#define REG_INTENA   0x14
#define REG_INTREQ   0x18

#define RING_ORDER 6
#define RING_N (1u << RING_ORDER)
#define RING_BYTES (0x100 + RING_N * 64 + RING_N * 16)

#define DIR_OPEN   1
#define DIR_CLOSE  2
#define DIR_LIST   3
#define OP_CANCEL  0xffff

#define REG_CLASS    0x20

/* Every Dalsin board with the services block: the services card, CPU cores,
 * FPU or TPU boards. Each has its own rings and interrupt server; the
 * directory on each says what it offers. */
#define MAX_BOARDS 4
struct board {
    volatile ULONG *regs;
    UBYTE *memory, *ring;            /* ring is memory 4 KB aligned */
    ULONG sqTail, cqHead;
    struct Interrupt irq;
};
static struct board boards[MAX_BOARDS];
static int boardCount;

#define RING_U32(b, off) (*(volatile ULONG *)((b)->ring + (off)))
#define SQ_TAIL(b) RING_U32(b, 0x000)
#define SQ_HEAD(b) RING_U32(b, 0x040)
#define CQ_TAIL(b) RING_U32(b, 0x080)
#define CQ_HEAD(b) RING_U32(b, 0x0c0)
#define SQ_ENTRY(b, i) ((b)->ring + 0x100 + ((i) & (RING_N - 1)) * 64)
#define CQ_ENTRY(b, i) ((b)->ring + 0x100 + RING_N * 64 + ((i) & (RING_N - 1)) * 16)

static ULONG boardServer(REG(a1, struct board *b))
{
    if (b->regs[REG_INTREQ / 4] & 1) {
        b->regs[REG_INTREQ / 4] = 1;  /* clear first: a later completion raises it again */
        Signal(&worker->pr_Task, 1UL << cardSignal);
    }
    return 0;                         /* the PORTS chain is shared: let the others look */
}

static void cardStart(void)
{
    struct ConfigDev *cd = NULL;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    if (!ExpansionBase)
        return;
    while (boardCount < MAX_BOARDS && (cd = FindConfigDev(cd, OPENSERVICE_MANUFACTURER, -1))) {
        volatile ULONG *regs = (volatile ULONG *)cd->cd_BoardAddr;
        struct board *b = &boards[boardCount];
        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) || (cd->cd_Flags & CDF_SHUTUP)
            || regs[REG_MAGIC / 4] != OPENSERVICE_MAGIC || regs[REG_VERSION / 4] != 1)
            continue;
        b->memory = AllocMem(RING_BYTES + 4096, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        if (!b->memory)
            break;
        b->ring = (UBYTE *)(((ULONG)b->memory + 4095) & ~4095UL);
        b->regs = regs;
        b->sqTail = b->cqHead = 0;
        b->irq.is_Node.ln_Type = NT_INTERRUPT;
        b->irq.is_Node.ln_Name = (char *)DevName;
        b->irq.is_Data = b;
        b->irq.is_Code = (void (*)(void))boardServer;
        AddIntServer(INTB_PORTS, &b->irq);
        CacheClearU();
        regs[REG_ORDER / 4] = RING_ORDER;
        regs[REG_RINGS / 4] = (ULONG)b->ring;
        regs[REG_INTENA / 4] = 1;
        boardCount++;
    }
    CloseLibrary((struct Library *)ExpansionBase);
    ExpansionBase = NULL;
}

static void cardStop(void)
{
    while (boardCount) {
        struct board *b = &boards[--boardCount];
        b->regs[REG_INTENA / 4] = 0;
        b->regs[REG_RINGS / 4] = 0;
        RemIntServer(INTB_PORTS, &b->irq);
        FreeMem(b->memory, RING_BYTES + 4096);
        b->regs = NULL;
    }
}

/* ---- what is in flight ---------------------------------------------------- */

enum { WHERE_NONE, WHERE_CARD = OSWHERE_CARD, WHERE_LAN = OSWHERE_LAN };
enum { KIND_USER, KIND_OPEN, KIND_LIST, KIND_INTERNAL };

#define MAX_PENDING 64
#define MAX_HANDLES 32
#define MAX_CONNS 4

struct pending {
    struct OSRequest *io;            /* NULL: free, or internal */
    ULONG id;
    UBYTE kind, where, conn, cancelSent;
};
static struct pending pending[MAX_PENDING];
static ULONG nextId = 1;

struct handle {
    UBYTE where, conn;
    UWORD remote;
};
static struct handle handles[MAX_HANDLES];   /* local handle = index + 1 */

struct conn {
    LONG socket;                     /* -1: closed */
    ULONG address;
    UWORD port;
    UWORD users;                     /* handles on it */
};
static struct conn conns[MAX_CONNS];

static struct mdns_cradle cradles[8];
static int cradleCount;
static ULONG browsedAt;              /* seconds, 0: never */

static struct List waiting;          /* requests the card ring had no room for */
/* With ENV:OpenService/Debug present, the worker logs to T:openservice.log. */
static BPTR debugLog;
static void dbg(const char *what, ULONG a, ULONG b)
{
    static const char hex[] = "0123456789abcdef";
    char line[96];
    int n = 0, k;
    if (!debugLog)
        return;
    while (*what && n < 60)
        line[n++] = *what++;
    for (k = 0; k < 2; k++) {
        ULONG v = k ? b : a;
        int d;
        line[n++] = ' ';
        for (d = 28; d >= 0; d -= 4)
            line[n++] = hex[(v >> d) & 15];
    }
    line[n++] = '\n';
    Write(debugLog, line, n);
}


static struct pending *newPending(struct OSRequest *io, int kind, int where, int conn)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
        if (!pending[i].id) {
            struct pending *p = &pending[i];
            p->io = io;
            p->id = nextId++;
            if (!nextId)
                nextId = 1;
            p->kind = (UBYTE)kind;
            p->where = (UBYTE)where;
            p->conn = (UBYTE)conn;
            p->cancelSent = 0;
            return p;
        }
    return NULL;
}

static struct pending *findPending(ULONG id)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id == id)
            return &pending[i];
    return NULL;
}

static void finish(struct OSRequest *io, LONG status, ULONG result, ULONG aux)
{
    io->os_Status = status;
    io->os_Result = result;
    io->os_Aux = aux;
    io->os_Req.io_Error = status == OSERR_CANCELLED ? IOERR_ABORTED : 0;
    ReplyMsg(&io->os_Req.io_Message);
}

/* The 64-byte entry for io, as the card and the LAN both take it. */
static void putEntry(UBYTE *e, ULONG id, UWORD service, UWORD op, const struct OSRequest *io)
{
    ULONG *l = (ULONG *)e;
    int i;
    l[0] = id;
    ((UWORD *)e)[2] = service;
    ((UWORD *)e)[3] = op;
    l[2] = io ? io->os_Flags : 0;
    l[3] = io ? io->os_Arg : 0;
    for (i = 0; i < 4; i++) {
        l[4 + 2 * i] = io ? (ULONG)io->os_Buf[i].ob_Data : 0;
        l[5 + 2 * i] = io ? io->os_Buf[i].ob_Length : 0;
    }
    for (i = 0; i < 4; i++)
        l[12 + i] = io ? io->os_Extra[i] : 0;
}

/* ---- the card path ---------------------------------------------------------- */

static int cardSubmit(struct pending *p, UWORD service, UWORD op, const struct OSRequest *io, ULONG arg)
{
    struct board *b = &boards[p->conn];
    UBYTE *e;
    int i;
    if (b->sqTail - SQ_HEAD(b) >= RING_N)
        return 0;
    e = SQ_ENTRY(b, b->sqTail);
    putEntry(e, p->id, service, op, io);
    if (!io)
        ((ULONG *)e)[3] = arg;
    for (i = 0; io && i < 4; i++)
        if (io->os_Buf[i].ob_Length) {
            ULONG len = io->os_Buf[i].ob_Length;
            CachePreDMA(io->os_Buf[i].ob_Data, &len, io->os_Flags & (1UL << i) ? 0 : DMA_ReadFromRAM);
        }
    b->sqTail++;
    SQ_TAIL(b) = b->sqTail;
    CacheClearU();
    b->regs[REG_DOORBELL / 4] = 1;
    return 1;
}

static void completed(struct pending *p, LONG status, ULONG result, ULONG aux);
static void listNext(struct OSRequest *io);

static void cardDrain(void)
{
    int k;
    CacheClearU();
    for (k = 0; k < boardCount; k++) {
        struct board *b = &boards[k];
        ULONG tail;
        while (b->cqHead != (tail = CQ_TAIL(b))) {
            while (b->cqHead != tail) {
                ULONG *c = (ULONG *)CQ_ENTRY(b, b->cqHead);
                struct pending *p = findPending(c[0]);
                LONG status = (LONG)c[1];
                ULONG result = c[2], aux = c[3];
                b->cqHead++;
                if (p && p->where == WHERE_CARD && p->conn == k)
                    completed(p, status, result, aux);
            }
            CQ_HEAD(b) = b->cqHead;
            CacheClearU();
        }
    }
}

/* ---- the LAN path ------------------------------------------------------------ */

static int sendAll(LONG s, const void *data, ULONG length)
{
    const UBYTE *p = data;
    while (length) {
        LONG n = send(s, (APTR)p, length, 0);
        if (n <= 0)
            return 0;
        p += n;
        length -= n;
    }
    return 1;
}

static int recvAll(LONG s, void *data, ULONG length)
{
    UBYTE *p = data;
    while (length) {
        LONG n = recv(s, p, length, 0);
        if (n <= 0)
            return 0;
        p += n;
        length -= n;
    }
    return 1;
}

static void connLost(int c);

static int lanSubmit(struct pending *p, int c, UWORD service, UWORD op, const struct OSRequest *io, ULONG arg)
{
    UBYTE e[64];
    int i;
    putEntry(e, p->id, service, op, io);
    if (!io)
        ((ULONG *)e)[3] = arg;
    if (!sendAll(conns[c].socket, e, sizeof e))
        return 0;
    for (i = 0; io && i < 4; i++)
        if (io->os_Buf[i].ob_Length && !(io->os_Flags & (1UL << i))
            && !sendAll(conns[c].socket, io->os_Buf[i].ob_Data, io->os_Buf[i].ob_Length))
            return 0;
    return 1;
}

/* One answer from connection c: the completion, then the written buffers. */
static void lanReceive(int c)
{
    ULONG head[4];
    struct pending *p;
    int i;
    dbg("lanReceive", c, conns[c].socket);
    if (!recvAll(conns[c].socket, head, sizeof head)) {
        connLost(c);
        return;
    }
    p = findPending(head[0]);
    for (i = 0; i < 4; i++) {
        struct OSRequest *io = p ? p->io : NULL;
        ULONG want, room, n;
        if (!io || !io->os_Buf[i].ob_Length || !(io->os_Flags & (1UL << i)))
            continue;
        if (!recvAll(conns[c].socket, &want, 4)) {
            connLost(c);
            return;
        }
        room = io->os_Buf[i].ob_Length;
        n = want < room ? want : room;
        if (!recvAll(conns[c].socket, io->os_Buf[i].ob_Data, n)) {
            connLost(c);
            return;
        }
        for (; n < want; n++) {          /* more than fits: read and drop */
            UBYTE drop;
            if (!recvAll(conns[c].socket, &drop, 1)) {
                connLost(c);
                return;
            }
        }
    }
    dbg("lan answer id/status", head[0], head[1]);
    if (p)
        completed(p, (LONG)head[1], head[2], head[3]);
}

static int connect3s(ULONG address, UWORD port)
{
    struct sockaddr_in to;
    LONG s = socket(AF_INET, SOCK_STREAM, 0), one = 1, zero = 0, err = 0;
    socklen_t errLength = sizeof err;
    fd_set ready;
    struct timeval wait;
    if (s < 0)
        return -1;
    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = address;
    IoctlSocket(s, FIONBIO, (char *)&one);
    if (connect(s, (struct sockaddr *)&to, sizeof to) < 0) {
        FD_ZERO(&ready);
        FD_SET(s, &ready);
        wait.tv_sec = 3;
        wait.tv_usec = 0;
        if (WaitSelect(s + 1, NULL, &ready, NULL, &wait, NULL) <= 0
            || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errLength) < 0 || err) {
            CloseSocket(s);
            return -1;
        }
    }
    IoctlSocket(s, FIONBIO, (char *)&zero);
    setsockopt(s, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
    return s;
}

/* The connection to a paired Cradle offering name, opened when needed. */
static int lanFor(const char *name)
{
    struct DateStamp ds;
    ULONG now;
    int i, c;
    if (!SocketBase)
        return -1;
    {
        /* No Cradle paired: nothing on the LAN to use, so no question asked. */
        BPTR paired = Lock((CONST_STRPTR)OPENSERVICE_PAIRED_FILE, ACCESS_READ);
        if (!paired)
            return -1;
        UnLock(paired);
    }
    DateStamp(&ds);
    now = (ULONG)ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    if (!browsedAt || now - browsedAt > 30) {
        cradleCount = mdns_browse(1, cradles, 8, 1UL << quitSignal);
        if (cradleCount < 0)
            cradleCount = 0;
        browsedAt = now ? now : 1;
    }
    for (i = 0; i < cradleCount; i++) {
        struct mdns_cradle *cr = &cradles[i];
        int free = -1;
        if (!cr->address || !mdns_offers(cr, name) || !openservice_paired(cr->fingerprint))
            continue;
        for (c = 0; c < MAX_CONNS; c++) {
            if (conns[c].socket >= 0 && conns[c].address == cr->address && conns[c].port == cr->port)
                return c;
            if (conns[c].socket < 0 && free < 0)
                free = c;
        }
        if (free < 0)
            return -1;
        conns[free].socket = connect3s(cr->address, cr->port);
        if (conns[free].socket < 0)
            continue;
        conns[free].address = cr->address;
        conns[free].port = cr->port;
        conns[free].users = 0;
        return free;
    }
    return -1;
}

static void connLost(int c)
{
    int i;
    if (conns[c].socket >= 0)
        CloseSocket(conns[c].socket);
    conns[c].socket = -1;
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id && pending[i].where == WHERE_LAN && pending[i].conn == c)
            completed(&pending[i], OSERR_LOST, 0, 0);
    for (i = 0; i < MAX_HANDLES; i++)
        if (handles[i].where == WHERE_LAN && handles[i].conn == c)
            handles[i].where = WHERE_NONE;
    browsedAt = 0;
}

/* ---- requests ------------------------------------------------------------------- */

static void completed(struct pending *p, LONG status, ULONG result, ULONG aux)
{
    struct OSRequest *io = p->io;
    int i;
    if (io && p->where == WHERE_CARD)
        for (i = 0; i < 4; i++)
            if (io->os_Buf[i].ob_Length) {
                ULONG len = io->os_Buf[i].ob_Length;
                CachePostDMA(io->os_Buf[i].ob_Data, &len, io->os_Flags & (1UL << i) ? 0 : DMA_ReadFromRAM);
            }
    if (io && p->kind == KIND_LIST) {
        p->id = 0;
        p->io = NULL;
        if (status == OSERR_OK)
            io->os_Private[2] += result;
        io->os_Private[3]++;
        listNext(io);
        return;
    }
    if (io && p->kind == KIND_OPEN) {
        if (status == OSERR_OK) {
            for (i = 0; i < MAX_HANDLES && handles[i].where; i++)
                ;
            if (i == MAX_HANDLES)
                status = OSERR_HOST;
            else {
                handles[i].where = p->where;
                handles[i].conn = p->conn;
                handles[i].remote = (UWORD)result;
                if (p->where == WHERE_LAN)
                    conns[p->conn].users++;
                result = i + 1;
            }
        } else if (status == OSERR_NOSERVICE && p->where == WHERE_CARD && p->conn + 1 < boardCount) {
            /* Not on this board: ask the next. */
            p->conn++;
            if (cardSubmit(p, 0, DIR_OPEN, io, 0))
                return;
        } else if (status == OSERR_NOSERVICE && p->where == WHERE_CARD) {
            /* On no board: try the nursery. */
            int c = lanFor((const char *)io->os_Buf[0].ob_Data);
            p->where = WHERE_LAN;
            p->conn = (UBYTE)c;
            if (c >= 0 && lanSubmit(p, c, 0, DIR_OPEN, io, 0))
                return;
        }
    }
    p->id = 0;
    p->io = NULL;
    if (io)
        finish(io, status, result, aux);
}

static void submit(struct pending *p, struct OSRequest *io, UWORD service, UWORD op)
{
    if (p->where == WHERE_CARD) {
        if (!cardSubmit(p, service, op, io, 0)) {
            p->id = 0;
            AddTail(&waiting, &io->os_Req.io_Message.mn_Node);
        }
    } else if (!lanSubmit(p, p->conn, service, op, io, 0)) {
        int c = p->conn;
        p->io = NULL;
        p->id = 0;
        finish(io, OSERR_LOST, 0, 0);
        connLost(c);
    }
}

static void doOpen(struct OSRequest *io)
{
    struct pending *p;
    char name[64];
    ULONG n = io->os_Buf[0].ob_Length;
    int c;
    if (!io->os_Buf[0].ob_Data || !n) {
        finish(io, OSERR_BADREQUEST, 0, 0);
        return;
    }
    if (n > sizeof name - 1)
        n = sizeof name - 1;
    memcpy(name, io->os_Buf[0].ob_Data, n);
    name[n] = 0;
    io->os_Flags = 0;
    if (boardCount) {
        p = newPending(io, KIND_OPEN, WHERE_CARD, 0);
        if (!p) {
            finish(io, OSERR_HOST, 0, 0);
            return;
        }
        submit(p, io, 0, DIR_OPEN);
        return;
    }
    c = lanFor(name);
    if (c < 0) {
        finish(io, OSERR_NOSERVICE, 0, 0);
        return;
    }
    p = newPending(io, KIND_OPEN, WHERE_LAN, c);
    if (!p) {
        finish(io, OSERR_HOST, 0, 0);
        return;
    }
    submit(p, io, 0, DIR_OPEN);
}

static void doClose(struct OSRequest *io)
{
    UWORD h = io->os_Service;
    struct handle *hd;
    struct pending *p;
    if (!h || h > MAX_HANDLES || !handles[h - 1].where) {
        finish(io, OSERR_BADREQUEST, 0, 0);
        return;
    }
    hd = &handles[h - 1];
    p = newPending(NULL, KIND_INTERNAL, hd->where, hd->conn);
    if (p) {
        if (hd->where == WHERE_CARD)
            cardSubmit(p, 0, DIR_CLOSE, NULL, hd->remote);
        else
            lanSubmit(p, hd->conn, 0, DIR_CLOSE, NULL, hd->remote);
    }
    if (hd->where == WHERE_LAN && conns[hd->conn].users)
        conns[hd->conn].users--;
    hd->where = WHERE_NONE;
    finish(io, OSERR_OK, 0, 0);
}

/* LIST asks each board in turn, each writing after the last (os_Private[2]
 * holds the bytes so far, os_Private[3] the board), then the nursery. */
static void listLan(struct OSRequest *io)
{
    char *out = io->os_Buf[0].ob_Data;
    ULONG room = io->os_Buf[0].ob_Length, used = io->os_Private[2];
    int i;
    if (!boardCount) {                 /* the nursery only when nothing is local */
        lanFor("");
        for (i = 0; i < cradleCount; i++) {
            const char *s = cradles[i].services;
            if (!openservice_paired(cradles[i].fingerprint))
                continue;
            while (*s && used < room) {
                out[used++] = *s == ',' ? 0 : *s;
                s++;
            }
            if (used < room)
                out[used++] = 0;
        }
    }
    finish(io, OSERR_OK, used, 0);
}

static void listNext(struct OSRequest *io)
{
    struct pending *p;
    ULONG k = io->os_Private[3];
    if (k >= (ULONG)boardCount) {
        listLan(io);
        return;
    }
    p = newPending(io, KIND_LIST, WHERE_CARD, (int)k);
    if (!p) {
        finish(io, OSERR_HOST, 0, 0);
        return;
    }
    {
        struct OSRequest part = *io;          /* the rest of the buffer */
        part.os_Flags = 1;
        part.os_Buf[0].ob_Data = (UBYTE *)io->os_Buf[0].ob_Data + io->os_Private[2];
        part.os_Buf[0].ob_Length = io->os_Buf[0].ob_Length - io->os_Private[2];
        if (!part.os_Buf[0].ob_Length || !cardSubmit(p, 0, DIR_LIST, &part, 0)) {
            p->id = 0;
            p->io = NULL;
            listLan(io);
        }
    }
}

static void doList(struct OSRequest *io)
{
    io->os_Flags = 1;
    io->os_Private[2] = 0;
    io->os_Private[3] = 0;
    listNext(io);
}

static void doCall(struct OSRequest *io)
{
    UWORD h = io->os_Service;
    struct handle *hd;
    struct pending *p;
    if (!h || h > MAX_HANDLES || !handles[h - 1].where) {
        finish(io, h && h <= MAX_HANDLES ? OSERR_LOST : OSERR_BADREQUEST, 0, 0);
        return;
    }
    hd = &handles[h - 1];
    p = newPending(io, KIND_USER, hd->where, hd->conn);
    if (!p) {
        finish(io, OSERR_HOST, 0, 0);
        return;
    }
    submit(p, io, hd->remote, io->os_Op);
}

static void dispatch(struct OSRequest *io)
{
    dbg("dispatch cmd", io->os_Req.io_Command, io->os_Service);
    if (io->os_Private[1]) {                       /* aborted before it started */
        finish(io, OSERR_CANCELLED, 0, 0);
        return;
    }
    switch (io->os_Req.io_Command) {
    case OSCMD_OPEN: doOpen(io); break;
    case OSCMD_CLOSE: doClose(io); break;
    case OSCMD_LIST: doList(io); break;
    case OSCMD_CALL: doCall(io); break;
    case OSCMD_WHERE: {
        UWORD h = io->os_Service;
        finish(io, OSERR_OK, h && h <= MAX_HANDLES ? handles[h - 1].where : 0, 0);
        break;
    }
    default:
        io->os_Req.io_Error = IOERR_NOCMD;
        ReplyMsg(&io->os_Req.io_Message);
    }
}

/* AbortIO marked these (os_Private[1]); tell their service. */
static void sendCancels(void)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++) {
        struct pending *p = &pending[i];
        struct pending *c;
        if (!p->id || !p->io || p->cancelSent || !p->io->os_Private[1])
            continue;
        p->cancelSent = 1;
        c = newPending(NULL, KIND_INTERNAL, p->where, p->conn);
        if (!c)
            continue;
        if (p->where == WHERE_CARD)
            cardSubmit(c, 0, OP_CANCEL, NULL, p->id);
        else
            lanSubmit(c, p->conn, 0, OP_CANCEL, NULL, p->id);
    }
}

/* Requests waiting for room on the card that AbortIO marked. */
static void cancelWaiting(void)
{
    struct Node *n, *next;
    for (n = waiting.lh_Head; (next = n->ln_Succ); n = next)
        if (((struct OSRequest *)n)->os_Private[1]) {
            Remove(n);
            finish((struct OSRequest *)n, OSERR_CANCELLED, 0, 0);
        }
}

static void retryWaiting(void)
{
    struct Node *n;
    while ((n = RemHead(&waiting)))
        dispatch((struct OSRequest *)n);
}

static void workerMain(void)
{
    struct MsgPort *port = CreateMsgPort();
    int i, running = 1;
    for (i = 0; i < MAX_CONNS; i++)
        conns[i].socket = -1;
    waiting.lh_Head = (struct Node *)&waiting.lh_Tail;
    waiting.lh_Tail = NULL;
    waiting.lh_TailPred = (struct Node *)&waiting.lh_Head;
    cardSignal = AllocSignal(-1);
    abortSignal = AllocSignal(-1);
    quitSignal = AllocSignal(-1);   /* not Ctrl-C: bsdsocket's WaitSelect takes that one for itself */
    {
        BPTR probe = Lock((CONST_STRPTR)"ENV:OpenService/Debug", ACCESS_READ);
        if (probe) {
            UnLock(probe);
            debugLog = Open((CONST_STRPTR)"T:openservice.log", MODE_NEWFILE);
        }
    }
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    dbg("socketbase", (ULONG)SocketBase, 0);
    if (port && cardSignal >= 0 && abortSignal >= 0 && quitSignal >= 0)
        cardStart();
    Forbid();
    workerPort = port;
    Signal(starter, SIGF_SINGLE);
    if (!port || cardSignal < 0 || abortSignal < 0 || quitSignal < 0)
        running = 0;
    Permit();
    while (running) {
        ULONG mask = 1UL << port->mp_SigBit | 1UL << cardSignal | 1UL << abortSignal | 1UL << quitSignal;
        ULONG got;
        struct Message *m;
        fd_set ready;
        LONG top = -1;
        FD_ZERO(&ready);
        for (i = 0; i < MAX_CONNS; i++)
            if (conns[i].socket >= 0) {
                FD_SET(conns[i].socket, &ready);
                if (conns[i].socket > top)
                    top = conns[i].socket;
            }
        if (top >= 0) {
            /* A second at most, and signals checked again after: a Ctrl-C
             * that lands while WaitSelect sets up must not be lost. */
            struct timeval second;
            second.tv_sec = 1;
            second.tv_usec = 0;
            got = mask;
            LONG r;
            dbg("waitselect top/got", top, got);
            r = WaitSelect(top + 1, &ready, NULL, NULL, &second, &got);
            dbg("waitselect r/got", r, got);
            if (r > 0)
                for (i = 0; i < MAX_CONNS; i++)
                    if (conns[i].socket >= 0 && FD_ISSET(conns[i].socket, &ready))
                        lanReceive(i);
            got = (got & mask) | (SetSignal(0, mask) & mask);
        } else
            got = Wait(mask);
        if (got & 1UL << quitSignal)
            running = 0;
        if (boardCount && (got & 1UL << cardSignal)) {
            cardDrain();
            retryWaiting();
        }
        while ((m = GetMsg(port)))
            dispatch((struct OSRequest *)m);
        if (got & 1UL << abortSignal) {
            cancelWaiting();
            sendCancels();
        }
    }
    /* Everything still out completes as lost. */
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id)
            completed(&pending[i], OSERR_LOST, 0, 0);
    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].socket >= 0)
            CloseSocket(conns[i].socket);
    cardStop();
    if (SocketBase)
        CloseLibrary(SocketBase);
    SocketBase = NULL;
    if (debugLog)
        Close(debugLog);
    debugLog = 0;
    Forbid();
    if (port) {
        struct Message *m;
        while ((m = GetMsg(port)))
            ((struct IORequest *)m)->io_Error = IOERR_ABORTED, ReplyMsg(m);
        DeleteMsgPort(port);
    }
    workerPort = NULL;
    worker = NULL;
    Signal(starter, SIGF_SINGLE);
    /* Forbid() holds until this process ends. */
}

static int startWorker(void)
{
    if (worker)
        return workerPort != NULL;
    starter = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    worker = CreateNewProcTags(NP_Entry, (ULONG)workerMain, NP_Name, (ULONG)DevName,
                               NP_Priority, 5, NP_StackSize, 16384, TAG_DONE);
    if (!worker)
        return 0;
    Wait(SIGF_SINGLE);
    if (!workerPort) {
        worker = NULL;
        return 0;
    }
    return 1;
}

static void stopWorker(void)
{
    if (!worker)
        return;
    starter = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    Signal(&worker->pr_Task, 1UL << quitSignal);
    Wait(SIGF_SINGLE);
}

/* ---- the device ------------------------------------------------------------------ */

static struct Device *devInit(REG(d0, struct Device *dev), REG(a0, BPTR seg), REG(a6, struct ExecBase *sysBase))
{
    SysBase = sysBase;
    segList = seg;
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)DevName;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = DEV_VERSION;
    dev->dd_Library.lib_Revision = DEV_REVISION;
    dev->dd_Library.lib_IdString = (APTR)DevIdString;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (!DOSBase) {
        FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
        return NULL;
    }
    return dev;
}

static void devOpen(REG(a1, struct IORequest *io), REG(d0, ULONG unit), REG(d1, ULONG flags), REG(a6, struct Device *dev))
{
    (void)unit;
    (void)flags;
    dev->dd_Library.lib_OpenCnt++;               /* no expunge while the worker starts */
    if (!startWorker()) {
        dev->dd_Library.lib_OpenCnt--;
        io->io_Error = IOERR_OPENFAIL;
        return;
    }
    io->io_Device = dev;
    io->io_Unit = NULL;
    io->io_Error = 0;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    dev->dd_Library.lib_Flags &= ~LIBF_DELEXP;
}

static BPTR devExpunge(REG(a6, struct Device *dev))
{
    BPTR seg;
    if (dev->dd_Library.lib_OpenCnt) {
        dev->dd_Library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    stopWorker();
    Remove(&dev->dd_Library.lib_Node);
    CloseLibrary((struct Library *)DOSBase);
    seg = segList;
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

static BPTR devClose(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    io->io_Device = (struct Device *)-1;
    if (--dev->dd_Library.lib_OpenCnt == 0) {
        stopWorker();
        if (dev->dd_Library.lib_Flags & LIBF_DELEXP)
            return devExpunge(dev);
    }
    return 0;
}

static ULONG devNull(void)
{
    return 0;
}

static void devBeginIO(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    (void)dev;
    io->io_Flags &= ~IOF_QUICK;
    io->io_Error = 0;
    ((struct OSRequest *)io)->os_Private[1] = 0;
    if (!workerPort) {
        struct OSRequest *os = (struct OSRequest *)io;
        os->os_Status = OSERR_LOST;
        os->os_Result = os->os_Aux = 0;
        ReplyMsg(&io->io_Message);
        return;
    }
    PutMsg(workerPort, &io->io_Message);
}

static LONG devAbortIO(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    (void)dev;
    /* The worker finishes it: a queued request completes as cancelled, one
     * in flight once its service has been told. */
    Forbid();
    if (io->io_Message.mn_Node.ln_Type != NT_REPLYMSG) {
        ((struct OSRequest *)io)->os_Private[1] = 1;
        if (worker)
            Signal(&worker->pr_Task, 1UL << abortSignal);
    }
    Permit();
    return 0;
}

static const APTR funcTable[] = {
    (APTR)devOpen,
    (APTR)devClose,
    (APTR)devExpunge,
    (APTR)devNull,
    (APTR)devBeginIO,
    (APTR)devAbortIO,
    (APTR)-1
};

static const ULONG initTable[] = {
    sizeof(struct Device),
    (ULONG)funcTable,
    0,
    (ULONG)devInit
};

const struct Resident os_romtag __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&os_romtag,
    (APTR)(&os_romtag + 1),
    RTF_AUTOINIT,
    DEV_VERSION,
    NT_DEVICE,
    0,
    (char *)DevName,
    (char *)DevIdString,
    (APTR)initTable
};
