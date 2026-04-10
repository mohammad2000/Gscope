/*
 * error.c — Thread-local error state management
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ─── Thread-Local Error State ───────────────────────────────────── */

static __thread struct {
    gscope_err_t code;
    int          saved_errno;
    char         message[512];
} tls_error = { .code = GSCOPE_OK };

/* ─── Public API ─────────────────────────────────────────────────── */

const char *gscope_strerror(void)
{
    if (tls_error.code == GSCOPE_OK)
        return "success";
    if (tls_error.message[0] == '\0')
        return gscope_err_name(tls_error.code);
    return tls_error.message;
}

gscope_err_t gscope_last_error(void)
{
    return tls_error.code;
}

int gscope_last_errno(void)
{
    return tls_error.saved_errno;
}

const char *gscope_err_name(gscope_err_t err)
{
    switch (err) {
    case GSCOPE_OK:              return "GSCOPE_OK";
    case GSCOPE_ERR_INVAL:       return "GSCOPE_ERR_INVAL";
    case GSCOPE_ERR_NOMEM:       return "GSCOPE_ERR_NOMEM";
    case GSCOPE_ERR_PERM:        return "GSCOPE_ERR_PERM";
    case GSCOPE_ERR_EXIST:       return "GSCOPE_ERR_EXIST";
    case GSCOPE_ERR_NOENT:       return "GSCOPE_ERR_NOENT";
    case GSCOPE_ERR_STATE:       return "GSCOPE_ERR_STATE";
    case GSCOPE_ERR_BUSY:        return "GSCOPE_ERR_BUSY";
    case GSCOPE_ERR_TIMEOUT:     return "GSCOPE_ERR_TIMEOUT";
    case GSCOPE_ERR_IO:          return "GSCOPE_ERR_IO";
    case GSCOPE_ERR_SYSCALL:     return "GSCOPE_ERR_SYSCALL";
    case GSCOPE_ERR_NAMESPACE:   return "GSCOPE_ERR_NAMESPACE";
    case GSCOPE_ERR_CGROUP:      return "GSCOPE_ERR_CGROUP";
    case GSCOPE_ERR_NETWORK:     return "GSCOPE_ERR_NETWORK";
    case GSCOPE_ERR_NETLINK:     return "GSCOPE_ERR_NETLINK";
    case GSCOPE_ERR_ROOTFS:      return "GSCOPE_ERR_ROOTFS";
    case GSCOPE_ERR_MOUNT:       return "GSCOPE_ERR_MOUNT";
    case GSCOPE_ERR_PROCESS:     return "GSCOPE_ERR_PROCESS";
    case GSCOPE_ERR_SECCOMP:     return "GSCOPE_ERR_SECCOMP";
    case GSCOPE_ERR_CAPS:        return "GSCOPE_ERR_CAPS";
    case GSCOPE_ERR_SECURITY:    return "GSCOPE_ERR_SECURITY";
    case GSCOPE_ERR_USER:        return "GSCOPE_ERR_USER";
    case GSCOPE_ERR_QUOTA:       return "GSCOPE_ERR_QUOTA";
    case GSCOPE_ERR_UNSUPPORTED: return "GSCOPE_ERR_UNSUPPORTED";
    default:                     return "GSCOPE_ERR_UNKNOWN";
    }
}

/* ─── Internal API ───────────────────────────────────────────────── */

gscope_err_t gscope_set_error(gscope_err_t code, const char *fmt, ...)
{
    tls_error.code = code;
    tls_error.saved_errno = 0;

    va_list args;
    va_start(args, fmt);
    vsnprintf(tls_error.message, sizeof(tls_error.message), fmt, args);
    va_end(args);

    return code;
}

gscope_err_t gscope_set_error_errno(gscope_err_t code, const char *fmt, ...)
{
    int saved = errno;
    tls_error.code = code;
    tls_error.saved_errno = saved;

    va_list args;
    va_start(args, fmt);

    int written = vsnprintf(tls_error.message, sizeof(tls_error.message),
                            fmt, args);
    va_end(args);

    /* Append ": <strerror>" if there's room */
    if (written > 0 && (size_t)written < sizeof(tls_error.message) - 3) {
        size_t remaining = sizeof(tls_error.message) - (size_t)written;
        snprintf(tls_error.message + written, remaining, ": %s",
                 strerror(saved));
    }

    return code;
}

void gscope_clear_error(void)
{
    tls_error.code = GSCOPE_OK;
    tls_error.saved_errno = 0;
    tls_error.message[0] = '\0';
}
