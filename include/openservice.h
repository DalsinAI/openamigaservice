/*
 * openservice: hand work to a named service, on the service card or on a
 * Cradle in the nursery (the LAN), or do it here when neither is there.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#ifndef OPENSERVICE_H
#define OPENSERVICE_H

/* The services card (ACSV): a Zorro II board, 64 KB. The wire is in the
 * project's design/SERVICES_CARD.md (v1); the emulator side is AC090's. */
#define OPENSERVICE_MANUFACTURER     0xDA15   /* Dalsin */
#define OPENSERVICE_MANUFACTURER_OLD 2011     /* boot ROMs from before Dalsin */
#define OPENSERVICE_PRODUCT          7

/* The first longword at the board's base. */
#define OPENSERVICE_MAGIC 0x41435356UL        /* "ACSV" */

/* Fingerprints of the Cradles this Amiga has paired with, one per line. */
#define OPENSERVICE_PAIRED_FILE "ENV:OpenService/Paired"

/* 1 when this fingerprint is in OPENSERVICE_PAIRED_FILE. */
int openservice_paired(const char *fingerprint);

#endif
