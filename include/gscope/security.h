/*
 * gscope/security.h — Seccomp + Linux capabilities API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_SECURITY_H
#define GSCOPE_SECURITY_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Capability constants (Linux capability bit positions) ──────── */

#define GSCOPE_CAP_CHOWN            (1ULL << 0)
#define GSCOPE_CAP_DAC_OVERRIDE     (1ULL << 1)
#define GSCOPE_CAP_DAC_READ_SEARCH  (1ULL << 2)
#define GSCOPE_CAP_FOWNER           (1ULL << 3)
#define GSCOPE_CAP_FSETID           (1ULL << 4)
#define GSCOPE_CAP_KILL             (1ULL << 5)
#define GSCOPE_CAP_SETGID           (1ULL << 6)
#define GSCOPE_CAP_SETUID           (1ULL << 7)
#define GSCOPE_CAP_SETPCAP          (1ULL << 8)
#define GSCOPE_CAP_NET_BIND_SERVICE (1ULL << 10)
#define GSCOPE_CAP_NET_RAW          (1ULL << 13)
#define GSCOPE_CAP_SYS_CHROOT       (1ULL << 18)
#define GSCOPE_CAP_MKNOD            (1ULL << 27)
#define GSCOPE_CAP_AUDIT_WRITE      (1ULL << 29)
#define GSCOPE_CAP_SETFCAP          (1ULL << 31)

/* Default capability set for STANDARD isolation */
#define GSCOPE_CAPS_DEFAULT ( \
    GSCOPE_CAP_CHOWN | GSCOPE_CAP_DAC_OVERRIDE | GSCOPE_CAP_FOWNER | \
    GSCOPE_CAP_FSETID | GSCOPE_CAP_KILL | GSCOPE_CAP_SETGID | \
    GSCOPE_CAP_SETUID | GSCOPE_CAP_SETPCAP | \
    GSCOPE_CAP_NET_BIND_SERVICE | GSCOPE_CAP_SYS_CHROOT | \
    GSCOPE_CAP_MKNOD | GSCOPE_CAP_AUDIT_WRITE | GSCOPE_CAP_SETFCAP)

/* ─── Functions ──────────────────────────────────────────────────── */

/*
 * Apply a seccomp-bpf filter to the calling process.
 * Must be called after fork(), before exec().
 */
gscope_err_t gscope_seccomp_apply(gscope_seccomp_t profile,
                                   const char *custom_path);

/*
 * Set the effective/permitted/inheritable capability sets.
 * keep_mask: bitmask of GSCOPE_CAP_* to retain.
 * All other capabilities are dropped.
 */
gscope_err_t gscope_caps_set(uint64_t keep_mask);

/*
 * Drop all capabilities from the calling process.
 */
gscope_err_t gscope_caps_drop_all(void);

/*
 * Set PR_SET_NO_NEW_PRIVS on the calling process.
 * Prevents gaining new privileges via exec of setuid/setgid binaries.
 */
gscope_err_t gscope_no_new_privs(void);

/*
 * Get the default capability mask for an isolation level.
 */
uint64_t gscope_caps_default_mask(gscope_isolation_t level);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_SECURITY_H */
