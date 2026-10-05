/*
 * Nursery: lists the services this Amiga can hand work to.
 *
 * First the boards with the services block (Dalsin $DA15, any product): the
 * services card, CPU cores, FPU or TPU boards, virtual ones on AmigaChrome on
 * a PC or on our appliance, or real ones. Then any
 * Cradle on the LAN that offers services, found with one mDNS question for
 * _amigachrome._tcp.local. A Cradle lists what it offers in its TXT record:
 *   svc=opentls.key/1,media.decode/1
 *   fp=<pairing fingerprint>
 *   name=<what to call it>
 *
 * Usage: Nursery [TIME=<seconds>] [ALL]
 *   TIME  how long to listen for answers (default 2)
 *   ALL   also list Cradles that offer no services
 *
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "openservice.h"
#include "mdns.h"

const char version[] __attribute__((used)) = "$VER: Nursery 0.1 (5.10.2026)";

struct Library *SocketBase;
struct ExpansionBase *ExpansionBase;

#define MAX_FOUND 16

static struct mdns_cradle found[MAX_FOUND];
static int foundCount;

/* ---- the boards ---- */

static const char *className(ULONG c)
{
    switch (c) {
    case OPENSERVICE_CLASS_SERVICES: return "services card";
    case OPENSERVICE_CLASS_CORES: return "CPU cores";
    case OPENSERVICE_CLASS_FPU: return "FPU";
    case OPENSERVICE_CLASS_TPU: return "TPU";
    }
    return "board";
}

static void listBoards(void)
{
    struct ConfigDev *cd = NULL;
    int any = 0;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    if (!ExpansionBase)
        return;
    while ((cd = FindConfigDev(cd, OPENSERVICE_MANUFACTURER, -1))) {
        const volatile ULONG *regs = (const volatile ULONG *)cd->cd_BoardAddr;
        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) || (cd->cd_Flags & CDF_SHUTUP) || regs[0] != OPENSERVICE_MAGIC)
            continue;
        printf("Board:   %s (product %u) at $%08lx, version %lu%s\n", className(regs[8]), cd->cd_Rom.er_Product,
               (unsigned long)cd->cd_BoardAddr, (unsigned long)regs[1], regs[7] & 1 ? ", runs on this machine's host" : "");
        any = 1;
    }
    if (!any)
        printf("Board:   none\n");
    CloseLibrary((struct Library *)ExpansionBase);
}

/* ---- mDNS ---- */

static void listCradles(int all)
{
    int i, shown = 0;
    for (i = 0; i < foundCount; i++) {
        struct mdns_cradle *c = &found[i];
        char label[128];
        char *dot;
        if (!c->services[0] && !all)
            continue;
        strncpy(label, c->name[0] ? c->name : c->instance, sizeof label - 1);
        label[sizeof label - 1] = 0;
        if (!c->name[0] && (dot = strstr(label, "._amigachrome")))
            *dot = 0;
        printf("Cradle:  %s", label);
        if (c->address)
            printf(" at %lu.%lu.%lu.%lu:%u", (unsigned long)(c->address >> 24), (unsigned long)((c->address >> 16) & 255), (unsigned long)((c->address >> 8) & 255), (unsigned long)(c->address & 255), c->port);
        else if (c->target[0])
            printf(" at %s:%u", c->target, c->port);
        printf("\n         services: %s\n", c->services[0] ? c->services : "none");
        printf("         paired:   %s\n", openservice_paired(c->fingerprint) ? "yes" : "no");
        shown++;
    }
    if (!shown)
        printf("LAN:     no Cradle offering services\n");
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR)"TIME/N,ALL/S", args, NULL);
    int seconds = 2;
    if (!rd) {
        PrintFault(IoErr(), (CONST_STRPTR)"Nursery");
        return RETURN_FAIL;
    }
    if (args[0])
        seconds = (int)*(LONG *)args[0];
    if (seconds < 1)
        seconds = 1;
    listBoards();
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase)
        printf("LAN:     no network\n");
    else {
        foundCount = mdns_browse(seconds, found, MAX_FOUND, SIGBREAKF_CTRL_C);
        if (foundCount < 0) {
            printf("LAN:     no socket\n");
            foundCount = 0;
        }
        listCradles(args[1] != 0);
        CloseLibrary(SocketBase);
    }
    FreeArgs(rd);
    return RETURN_OK;
}
