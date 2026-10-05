/*
 * Running host tools for the services: LibreOffice, poppler, ImageMagick,
 * FluidSynth, sidplayfp. MIT, Copyright (c) 2026 Dalsin Limited.
 */
#ifndef HOSTRUN_H
#define HOSTRUN_H

#include <stddef.h>
#include <stdint.h>

/* Runs argv (found on PATH) with stdout to fd_out (-1: /dev/null) and
 * stderr to /dev/null; 0 when it exits 0. */
int hr_run(char *const argv[], int fd_out);
/* Runs argv and returns its stdout, malloc'd and NUL-ended, length in *len. */
char *hr_run_read(char *const argv[], size_t *len);
/* A new private directory for one request's files (path into out); 0 on success. */
int hr_tempdir(char *out, size_t room);
/* Removes the files in dir, then dir. */
void hr_rmdir(const char *dir);
/* Writes n bytes to path; 0 on success. */
int hr_write(const char *path, const uint8_t *d, uint32_t n);
/* Reads a whole file, malloc'd; NULL on failure. */
uint8_t *hr_read(const char *path, size_t *len);

#endif
