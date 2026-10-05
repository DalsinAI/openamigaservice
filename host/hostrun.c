/*
 * Running host tools for the services (hostrun.h).
 * MIT, Copyright (c) 2026 Dalsin Limited.
 */
#define _GNU_SOURCE
#include "hostrun.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int hr_run(char *const argv[], int fd_out)
{
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int st = -1, rc;

    posix_spawn_file_actions_init(&fa);
    if (fd_out >= 0)
        posix_spawn_file_actions_adddup2(&fa, fd_out, 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

char *hr_run_read(char *const argv[], size_t *len)
{
    FILE *f = tmpfile();
    char *out = NULL;
    long n;

    if (!f)
        return NULL;
    if (hr_run(argv, fileno(f)) == 0 && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0) {
        rewind(f);
        if ((out = malloc(n + 1)) && fread(out, 1, n, f) == (size_t)n) {
            out[n] = 0;
            if (len)
                *len = n;
        } else {
            free(out);
            out = NULL;
        }
    }
    fclose(f);
    return out;
}

int hr_tempdir(char *out, size_t room)
{
    const char *base = getenv("TMPDIR");
    snprintf(out, room, "%s/openservice-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(out) ? 0 : -1;
}

void hr_rmdir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];

    if (d) {
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                unlink(path);
            }
        closedir(d);
    }
    rmdir(dir);
}

int hr_write(const char *path, const uint8_t *d, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f)
        return -1;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

uint8_t *hr_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *out = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        if ((out = malloc(n)) && fread(out, 1, n, f) == (size_t)n)
            *len = n;
        else {
            free(out);
            out = NULL;
        }
    }
    fclose(f);
    return out;
}
