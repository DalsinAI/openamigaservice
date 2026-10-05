/*
 * ServiceTest: tries openservice.device end to end.
 *   lists the services, opens echo/1 (or NAME), says where it runs, sends
 *   COUNT echoes and checks each answer, then times them.
 *
 * Usage: ServiceTest [NAME=<service>] [COUNT=<n>]
 *
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "devices/openservice.h"

static const char version[] = "$VER: ServiceTest 0.1 (5.10.2026)";

static LONG run(struct OSRequest *io, UWORD command)
{
    io->os_Req.io_Command = command;
    DoIO((struct IORequest *)io);
    return io->os_Req.io_Error ? -100 - io->os_Req.io_Error : io->os_Status;
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR)"NAME,COUNT/N", args, NULL);
    const char *name = args[0] ? (const char *)args[0] : "echo/1";
    LONG count = args[1] ? *(LONG *)args[1] : 100, i, bad = 0, status;
    struct MsgPort *port = CreateMsgPort();
    struct OSRequest *io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
    static char list[512], in[256], out[256];
    struct DateStamp t0, t1;
    UWORD handle;
    (void)version;
    if (!rd || !io) {
        printf("ServiceTest: no memory or bad arguments\n");
        return RETURN_FAIL;
    }
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)io, 0)) {
        printf("cannot open %s (%d)\n", OPENSERVICE_NAME, io->os_Req.io_Error);
        return RETURN_FAIL;
    }
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = list;
    io->os_Buf[0].ob_Length = sizeof list;
    status = run(io, OSCMD_LIST);
    printf("list: status %ld:", (long)status);
    for (i = 0; status == 0 && i < (LONG)io->os_Result; i += strlen(list + i) + 1)
        printf(" %s", list + i);
    printf("\n");

    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = (APTR)name;
    io->os_Buf[0].ob_Length = strlen(name);
    status = run(io, OSCMD_OPEN);
    if (status) {
        printf("open %s: status %ld (no card or paired Cradle offers it)\n", name, (long)status);
        CloseDevice((struct IORequest *)io);
        return RETURN_WARN;
    }
    handle = (UWORD)io->os_Result;
    io->os_Service = handle;
    run(io, OSCMD_WHERE);
    printf("open %s: handle %u, on %s\n", name, handle,
           io->os_Result == OSWHERE_CARD ? "the card" : io->os_Result == OSWHERE_LAN ? "a Cradle on the LAN" : "?");

    DateStamp(&t0);
    for (i = 0; i < count; i++) {
        int n = 1 + (int)(i % (sizeof in - 1)), k;
        for (k = 0; k < n; k++)
            in[k] = (char)(i + k);
        memset(out, 0, sizeof out);
        memset(io->os_Buf, 0, sizeof io->os_Buf);
        io->os_Service = handle;
        io->os_Op = 1;
        io->os_Flags = 2;                       /* buffer 1 is written */
        io->os_Buf[0].ob_Data = in;
        io->os_Buf[0].ob_Length = n;
        io->os_Buf[1].ob_Data = out;
        io->os_Buf[1].ob_Length = sizeof out;
        status = run(io, OSCMD_CALL);
        if (status || io->os_Result != (ULONG)n || memcmp(in, out, n)) {
            if (bad++ < 5)
                printf("echo %ld: status %ld, result %lu, %s\n", (long)i, (long)status,
                       (unsigned long)io->os_Result, memcmp(in, out, n) ? "data differs" : "data ok");
        }
    }
    DateStamp(&t1);
    {
        LONG ticks = (t1.ds_Days - t0.ds_Days) * 86400 * 50 + (t1.ds_Minute - t0.ds_Minute) * 3000 + (t1.ds_Tick - t0.ds_Tick);
        printf("echo: %ld calls, %ld bad, %ld ms, %ld us each\n", (long)count, (long)bad, (long)ticks * 20,
               count ? (long)(ticks * 20000 / count) : 0L);
    }
    io->os_Service = handle;
    run(io, OSCMD_CLOSE);
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rd);
    return bad ? RETURN_WARN : RETURN_OK;
}
