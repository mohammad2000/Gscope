/*
 * gscope/error.h — Error codes and retrieval
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_ERROR_H
#define GSCOPE_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Error Codes ────────────────────────────────────────────────── */

typedef enum {
    GSCOPE_OK              =   0,

    /* General */
    GSCOPE_ERR_INVAL       =  -1,   /* Invalid argument */
    GSCOPE_ERR_NOMEM       =  -2,   /* Out of memory */
    GSCOPE_ERR_PERM        =  -3,   /* Permission denied */
    GSCOPE_ERR_EXIST       =  -4,   /* Already exists */
    GSCOPE_ERR_NOENT       =  -5,   /* Not found */
    GSCOPE_ERR_STATE       =  -6,   /* Invalid state for operation */
    GSCOPE_ERR_BUSY        =  -7,   /* Resource busy */
    GSCOPE_ERR_TIMEOUT     =  -8,   /* Operation timed out */
    GSCOPE_ERR_IO          =  -9,   /* I/O error */

    /* Syscall */
    GSCOPE_ERR_SYSCALL     = -10,   /* Generic syscall failure (check errno) */

    /* Subsystem-specific */
    GSCOPE_ERR_NAMESPACE   = -20,   /* Namespace operation failed */
    GSCOPE_ERR_CGROUP      = -21,   /* Cgroup operation failed */
    GSCOPE_ERR_NETWORK     = -22,   /* Network operation failed */
    GSCOPE_ERR_NETLINK     = -23,   /* Netlink communication error */
    GSCOPE_ERR_ROOTFS      = -24,   /* Rootfs/overlay operation failed */
    GSCOPE_ERR_MOUNT       = -25,   /* Mount/unmount failed */
    GSCOPE_ERR_PROCESS     = -26,   /* Process spawn/signal failed */
    GSCOPE_ERR_SECCOMP     = -27,   /* Seccomp filter error */
    GSCOPE_ERR_CAPS        = -28,   /* Capabilities error */
    GSCOPE_ERR_SECURITY    = -29,   /* General security error */
    GSCOPE_ERR_USER        = -30,   /* User management error */
    GSCOPE_ERR_QUOTA       = -31,   /* Quota exceeded */

    /* Compatibility */
    GSCOPE_ERR_UNSUPPORTED = -40,   /* Feature not supported on this kernel */
} gscope_err_t;

/* ─── Error Retrieval (thread-safe) ──────────────────────────────── */

/*
 * Get human-readable error message for the last error on this thread.
 * Returns pointer to thread-local buffer — valid until next gscope call
 * on the same thread.
 */
const char  *gscope_strerror(void);

/*
 * Get the last error code for this thread.
 */
gscope_err_t gscope_last_error(void);

/*
 * Get the captured errno from the last failed syscall.
 * Returns 0 if the last error was not a syscall failure.
 */
int          gscope_last_errno(void);

/*
 * Convert an error code to its name string.
 * Returns e.g. "GSCOPE_ERR_NAMESPACE" or "GSCOPE_OK".
 * Never returns NULL.
 */
const char  *gscope_err_name(gscope_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_ERROR_H */
