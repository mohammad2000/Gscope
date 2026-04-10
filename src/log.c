/*
 * log.c — Pluggable logging subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "internal.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

/* ─── Default Log Handler (stderr) ───────────────────────────────── */

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
};

static const char *level_colors[] = {
    "\033[37m",     /* TRACE: white */
    "\033[36m",     /* DEBUG: cyan */
    "\033[32m",     /* INFO:  green */
    "\033[33m",     /* WARN:  yellow */
    "\033[31m",     /* ERROR: red */
    "\033[35m",     /* FATAL: magenta */
    "",             /* OFF */
};

static void default_log_fn(gscope_log_level_t level,
                            const char *file,
                            int line,
                            const char *fmt,
                            va_list args,
                            void *userdata)
{
    (void)userdata;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

    /* Strip path prefix — keep only filename */
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    /* Print header */
    fprintf(stderr, "%s%s %s%-5s\033[0m %s:%d: ",
            "\033[90m", timebuf,
            level_colors[level], level_names[level],
            basename, line);

    /* Print message */
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

/* ─── Logging Function ───────────────────────────────────────────── */

void gscope_log(gscope_ctx_t *ctx, gscope_log_level_t level,
                const char *file, int line, const char *fmt, ...)
{
    gscope_log_fn fn;
    void *userdata;
    gscope_log_level_t min_level;

    if (ctx) {
        fn = ctx->log_fn ? ctx->log_fn : default_log_fn;
        userdata = ctx->log_userdata;
        min_level = ctx->log_level;
    } else {
        fn = default_log_fn;
        userdata = NULL;
        min_level = GSCOPE_LOG_INFO;
    }

    if (level < min_level)
        return;

    va_list args;
    va_start(args, fmt);
    fn(level, file, line, fmt, args, userdata);
    va_end(args);
}
