/*
 * sec/caps.c — Linux capabilities management
 *
 * Linux capabilities split root's power into ~40 individual bits.
 * Instead of running as full root, a process can hold only the
 * specific capabilities it needs.
 *
 * We use raw capget/capset syscalls (no libcap dependency).
 * Version 3 header supports capabilities up to bit 63.
 *
 * Capability sets:
 *   Effective   — what the process can do RIGHT NOW
 *   Permitted   — max set of caps the process can gain
 *   Inheritable — caps passed to exec'd processes
 *
 * Our approach: set all three sets to the same mask (keep_mask).
 * Everything not in keep_mask is dropped.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <gscope/security.h>

#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_caps_set(uint64_t keep_mask)
{
#ifdef __linux__
    struct gscope_cap_header hdr = {
        .version = GSCOPE_LINUX_CAPABILITY_VERSION_3,
        .pid = 0,  /* 0 = current process */
    };

    /*
     * Version 3 uses TWO data structs:
     *   data[0] — capabilities 0-31
     *   data[1] — capabilities 32-63
     */
    struct gscope_cap_data data[2];
    memset(data, 0, sizeof(data));

    /* Split 64-bit mask into two 32-bit halves */
    uint32_t low  = (uint32_t)(keep_mask & 0xFFFFFFFF);
    uint32_t high = (uint32_t)((keep_mask >> 32) & 0xFFFFFFFF);

    /* Set all three capability sets to the same mask */
    data[0].effective   = low;
    data[0].permitted   = low;
    data[0].inheritable = low;

    data[1].effective   = high;
    data[1].permitted   = high;
    data[1].inheritable = high;

    if (gscope_capset(&hdr, data) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_CAPS,
                                      "capset() failed");

    /*
     * Also set the ambient capabilities for each bit in keep_mask.
     * Ambient caps are inherited across execve even without setuid.
     * This is important for non-root users inside the scope.
     */
    for (int bit = 0; bit < 64; bit++) {
        if (keep_mask & (1ULL << bit)) {
            /* PR_CAP_AMBIENT_RAISE — add to ambient set */
            prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, bit, 0, 0);
            /* Ignore errors — some caps may not be ambient-capable */
        }
    }

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)keep_mask;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "capabilities require Linux");
#endif
}

gscope_err_t gscope_caps_drop_all(void)
{
    return gscope_caps_set(0);
}

uint64_t gscope_caps_default_mask(gscope_isolation_t level)
{
    switch (level) {
    case GSCOPE_ISOLATION_MINIMAL:
        /* Almost full capabilities */
        return GSCOPE_CAPS_DEFAULT |
               GSCOPE_CAP_NET_RAW |
               GSCOPE_CAP_DAC_READ_SEARCH;

    case GSCOPE_ISOLATION_STANDARD:
        return GSCOPE_CAPS_DEFAULT;

    case GSCOPE_ISOLATION_HIGH:
        /* Reduced set — no mknod, no net_raw */
        return GSCOPE_CAP_CHOWN |
               GSCOPE_CAP_DAC_OVERRIDE |
               GSCOPE_CAP_FOWNER |
               GSCOPE_CAP_FSETID |
               GSCOPE_CAP_KILL |
               GSCOPE_CAP_SETGID |
               GSCOPE_CAP_SETUID |
               GSCOPE_CAP_NET_BIND_SERVICE |
               GSCOPE_CAP_AUDIT_WRITE;

    case GSCOPE_ISOLATION_MAXIMUM:
        /* Minimal — only what's absolutely needed */
        return GSCOPE_CAP_KILL |
               GSCOPE_CAP_SETGID |
               GSCOPE_CAP_SETUID |
               GSCOPE_CAP_NET_BIND_SERVICE;

    default:
        return GSCOPE_CAPS_DEFAULT;
    }
}
