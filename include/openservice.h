/*
 * openservice: hand work to a named service, on the service card or on a
 * Cradle in the nursery (the LAN), or do it here when neither is there.
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#ifndef OPENSERVICE_H
#define OPENSERVICE_H

/* Boards with the services block (ACSV at their base): Dalsin boards of any
 * product. The wire is the project's design/SERVICES_CARD.md (v1); the
 * emulator side is AC090's. */
#define OPENSERVICE_MANUFACTURER     0xDA15   /* Dalsin */

/* The CLASS register ($20): what kind of board it is. Only for showing;
 * each board's directory says what it offers. */
#define OPENSERVICE_CLASS_SERVICES 1
#define OPENSERVICE_CLASS_CORES    2
#define OPENSERVICE_CLASS_FPU      3
#define OPENSERVICE_CLASS_TPU      4

/* The first longword at the board's base. */
#define OPENSERVICE_MAGIC 0x41435356UL        /* "ACSV" */

/* Fingerprints of the Cradles this Amiga has paired with, one per line. */
#define OPENSERVICE_PAIRED_FILE "ENV:OpenService/Paired"

/* 1 when this fingerprint is in OPENSERVICE_PAIRED_FILE. */
int openservice_paired(const char *fingerprint);

#endif
