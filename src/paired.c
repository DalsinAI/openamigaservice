/*
 * The list of Cradles this Amiga has paired with (openservice.h). Plain
 * dos.library, so the device can use it too.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "openservice.h"

int openservice_paired(const char *fingerprint)
{
    char line[128];
    int paired = 0;
    BPTR f;
    if (!fingerprint || !fingerprint[0])
        return 0;
    f = Open((CONST_STRPTR)OPENSERVICE_PAIRED_FILE, MODE_OLDFILE);
    if (!f)
        return 0;
    while (!paired && FGets(f, (STRPTR)line, sizeof line)) {
        line[strcspn(line, " \t\r\n")] = 0;
        paired = !strcmp(line, fingerprint);
    }
    Close(f);
    return paired;
}
