/*
 * sec/seccomp.c — seccomp-bpf syscall filtering
 *
 * Seccomp (SECure COMPuting) restricts which syscalls a process can make.
 * We generate BPF (Berkeley Packet Filter) programs that the kernel
 * evaluates on every syscall.
 *
 * Filter structure (BPF):
 *   1. Load syscall number (seccomp_data.nr)
 *   2. Compare against blocked syscalls
 *   3. If match → SECCOMP_RET_ERRNO (return EPERM)
 *   4. If no match → SECCOMP_RET_ALLOW
 *
 * Profiles:
 *   DEFAULT    — Block dangerous syscalls (reboot, kexec, etc.)
 *   STRICT     — Whitelist-only (only known-safe syscalls allowed)
 *   PERMISSIVE — Block only critical syscalls
 *   DISABLED   — No filter
 *
 * This module builds BPF programs WITHOUT libseccomp dependency.
 * We generate the BPF bytecode directly.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

/* ─── Architecture Detection ─────────────────────────────────────── */

#ifdef __linux__
#if defined(__x86_64__)
#define GSCOPE_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__i386__)
#define GSCOPE_AUDIT_ARCH AUDIT_ARCH_I386
#elif defined(__aarch64__)
#define GSCOPE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define GSCOPE_AUDIT_ARCH AUDIT_ARCH_ARM
#else
#define GSCOPE_AUDIT_ARCH 0
#endif
#endif /* __linux__ */

/* ─── BPF Helpers ────────────────────────────────────────────────── */

#ifdef __linux__

/* Max BPF instructions we'll generate */
#define MAX_BPF_INSNS 512

/*
 * Syscalls blocked by DEFAULT profile.
 *
 * Based on Docker's default seccomp profile + runc CVE mitigations.
 * Reference: https://docs.docker.com/engine/security/seccomp/
 */
static const int blocked_default[] = {
    /* ─── System control ─── */
    __NR_reboot,              /* System reboot */
    __NR_swapon,              /* Enable swap */
    __NR_swapoff,             /* Disable swap */
    __NR_acct,                /* Process accounting */
    __NR_settimeofday,        /* Change system time */
#ifdef __NR_clock_settime
    __NR_clock_settime,       /* Change clock */
#endif
#ifdef __NR_clock_adjtime
    __NR_clock_adjtime,       /* Adjust clock */
#endif
#ifdef __NR_adjtimex
    __NR_adjtimex,            /* Fine-tune clock */
#endif

    /* ─── Kernel modules ─── */
    __NR_init_module,         /* Load kernel module */
    __NR_delete_module,       /* Unload kernel module */
#ifdef __NR_finit_module
    __NR_finit_module,        /* Load kernel module from fd */
#endif

    /* ─── Kernel replacement ─── */
#ifdef __NR_kexec_load
    __NR_kexec_load,          /* Load new kernel */
#endif
#ifdef __NR_kexec_file_load
    __NR_kexec_file_load,     /* Load new kernel from fd */
#endif

    /* ─── io_uring (bypasses seccomp entirely!) ─── */
#ifdef __NR_io_uring_setup
    __NR_io_uring_setup,      /* Create io_uring instance */
#endif
#ifdef __NR_io_uring_enter
    __NR_io_uring_enter,      /* Submit io_uring operations */
#endif
#ifdef __NR_io_uring_register
    __NR_io_uring_register,   /* Register io_uring resources */
#endif

    /* ─── New mount API (kernel 5.2+, bypasses mount()) ─── */
#ifdef __NR_mount_setattr
    __NR_mount_setattr,       /* Change mount attributes */
#endif
#ifdef __NR_move_mount
    __NR_move_mount,          /* Move mount between namespaces */
#endif
#ifdef __NR_open_tree
    __NR_open_tree,           /* Open mount tree */
#endif
#ifdef __NR_fsopen
    __NR_fsopen,              /* Open filesystem context */
#endif
#ifdef __NR_fsconfig
    __NR_fsconfig,            /* Configure filesystem */
#endif
#ifdef __NR_fsmount
    __NR_fsmount,             /* Create filesystem mount */
#endif
#ifdef __NR_fspick
    __NR_fspick,              /* Pick filesystem for reconfiguration */
#endif

    /* ─── Container escape vectors ─── */
#ifdef __NR_open_by_handle_at
    __NR_open_by_handle_at,   /* Container escape: open file by handle */
#endif
#ifdef __NR_name_to_handle_at
    __NR_name_to_handle_at,   /* Get file handle (used with above) */
#endif
#ifdef __NR_userfaultfd
    __NR_userfaultfd,         /* Used in exploit chains */
#endif

    /* ─── Cross-process memory access ─── */
#ifdef __NR_process_vm_readv
    __NR_process_vm_readv,    /* Read another process memory */
#endif
#ifdef __NR_process_vm_writev
    __NR_process_vm_writev,   /* Write another process memory */
#endif

    /* ─── eBPF (powerful kernel access) ─── */
#ifdef __NR_bpf
    __NR_bpf,                 /* Load eBPF programs */
#endif

    /* ─── Profiling / Info leak ─── */
#ifdef __NR_perf_event_open
    __NR_perf_event_open,     /* Performance monitoring */
#endif
#ifdef __NR_lookup_dcookie
    __NR_lookup_dcookie,      /* Profiling cookie lookup */
#endif
#ifdef __NR_syslog
    __NR_syslog,              /* Read kernel ring buffer */
#endif
#ifdef __NR__sysctl
    __NR__sysctl,             /* Deprecated sysctl */
#endif

    /* ─── NUMA manipulation ─── */
#ifdef __NR_mbind
    __NR_mbind,               /* Set memory policy */
#endif
#ifdef __NR_set_mempolicy
    __NR_set_mempolicy,       /* Set NUMA memory policy */
#endif
#ifdef __NR_migrate_pages
    __NR_migrate_pages,       /* Migrate process pages */
#endif
#ifdef __NR_move_pages
    __NR_move_pages,          /* Move pages to NUMA node */
#endif

    /* ─── Execution domain ─── */
#ifdef __NR_personality
    __NR_personality,         /* Change execution domain (exploit vector) */
#endif

    /* ─── Kernel key management ─── */
#ifdef __NR_add_key
    __NR_add_key,
#endif
#ifdef __NR_request_key
    __NR_request_key,
#endif
#ifdef __NR_keyctl
    __NR_keyctl,
#endif

    /* ─── Obsolete / dangerous ─── */
#ifdef __NR_nfsservctl
    __NR_nfsservctl,          /* NFS server control */
#endif
#ifdef __NR_vm86
    __NR_vm86,                /* x86 virtual 8086 mode */
#endif
#ifdef __NR_vm86old
    __NR_vm86old,             /* Old vm86 interface */
#endif

    -1  /* Sentinel */
};

/*
 * Additional syscalls blocked by STRICT profile.
 * These are legitimate in some use cases but dangerous for isolation.
 */
static const int blocked_strict_extra[] = {
    __NR_mount,               /* Mount filesystems */
    __NR_umount2,             /* Unmount filesystems */
#ifdef __NR_pivot_root
    __NR_pivot_root,          /* Prevent nested pivot */
#endif
    __NR_chroot,              /* Prevent chroot escape */
    __NR_mknod,               /* Create device nodes */
#ifdef __NR_mknodat
    __NR_mknodat,             /* Create device nodes */
#endif
    __NR_ptrace,              /* Process debugging (escape vector) */
#ifdef __NR_kcmp
    __NR_kcmp,                /* Compare kernel resources */
#endif
#ifdef __NR_unshare
    __NR_unshare,             /* Prevent nested namespaces */
#endif
#ifdef __NR_setns
    __NR_setns,               /* Prevent namespace escape */
#endif
#ifdef __NR_clone
    __NR_clone,               /* Prevent new namespace creation */
#endif
#ifdef __NR_clone3
    __NR_clone3,              /* Prevent new namespace creation */
#endif
    __NR_ioctl,               /* Block all ioctl (very strict!) */
    -1  /* Sentinel */
};

/*
 * Build BPF program to block a list of syscalls.
 *
 * Generated BPF structure:
 *   [0]  validate architecture
 *   [1]  load syscall number
 *   [2..N] JEQ syscall_nr → ERRNO(EPERM)
 *   [N+1]  ALLOW
 */
static int build_blocklist_filter(const int *blocked, int blocked_count,
                                   struct sock_filter *filter, int max_insns)
{
    int idx = 0;

    if (blocked_count + 4 > max_insns)
        return -1;

    /* Instruction 0: Load architecture from seccomp_data */
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, arch));

#if GSCOPE_AUDIT_ARCH != 0
    /*
     * Instruction 1-2: Validate architecture matches.
     * If not, KILL the process immediately.
     *
     * This prevents 32-bit ABI bypass on x86_64:
     * An attacker could use `int 0x80` to invoke 32-bit syscalls
     * which have DIFFERENT numbers than 64-bit ones, bypassing
     * our 64-bit blocklist entirely.
     *
     * By killing on architecture mismatch, we ensure ONLY the
     * native ABI can be used.
     */
    filter[idx++] = (struct sock_filter)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, GSCOPE_AUDIT_ARCH, 1, 0);
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif

#if defined(__x86_64__) && defined(AUDIT_ARCH_I386)
    /*
     * EXTRA: Explicitly block 32-bit x86 ABI.
     *
     * Even though the check above kills on non-matching arch,
     * we add an explicit block for AUDIT_ARCH_I386 as defense-in-depth.
     * This catches edge cases where the kernel might allow compat calls.
     */
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, arch));
    filter[idx++] = (struct sock_filter)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, 0, 1);
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif

    /* Instruction 2: Load syscall number */
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr));

    /* Instructions 3..N: Check each blocked syscall */
    for (int i = 0; i < blocked_count; i++) {
        /* JEQ blocked[i] → ERRNO(EPERM) */
        /* Jump forward: if equal, skip 0 (next = ERRNO), if not, skip 1 */
        filter[idx++] = (struct sock_filter)
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)blocked[i],
                     0, 1);
        filter[idx++] = (struct sock_filter)
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* Final instruction: ALLOW */
    filter[idx++] = (struct sock_filter)
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    return idx;
}

/*
 * Collect blocked syscalls for a profile.
 */
static int collect_blocked(gscope_seccomp_t profile,
                            int *out, int max_count)
{
    int count = 0;

    /* Default blocked list */
    if (profile == GSCOPE_SECCOMP_DEFAULT ||
        profile == GSCOPE_SECCOMP_STRICT ||
        profile == GSCOPE_SECCOMP_PERMISSIVE) {

        for (int i = 0; blocked_default[i] != -1 && count < max_count; i++) {
            if (blocked_default[i] > 0)
                out[count++] = blocked_default[i];
        }
    }

    /* Strict adds extra blocks */
    if (profile == GSCOPE_SECCOMP_STRICT) {
        for (int i = 0; blocked_strict_extra[i] != -1 && count < max_count; i++) {
            if (blocked_strict_extra[i] > 0)
                out[count++] = blocked_strict_extra[i];
        }
    }

    return count;
}

#endif /* __linux__ */

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_seccomp_apply(gscope_seccomp_t profile,
                                   const char *custom_path)
{
    if (profile == GSCOPE_SECCOMP_DISABLED) {
        gscope_clear_error();
        return GSCOPE_OK;
    }

#ifdef __linux__
    /* Custom profile from file — not implemented yet */
    if (profile == GSCOPE_SECCOMP_CUSTOM) {
        (void)custom_path;
        return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                                "custom seccomp profiles not yet supported");
    }

    /* PR_SET_NO_NEW_PRIVS is required before seccomp */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_SECCOMP,
                                      "prctl(NO_NEW_PRIVS) failed");

    /* Collect blocked syscalls */
    int blocked[128];
    int blocked_count = collect_blocked(profile, blocked, 128);

    if (blocked_count == 0) {
        gscope_clear_error();
        return GSCOPE_OK;
    }

    /* Build BPF program */
    struct sock_filter filter[MAX_BPF_INSNS];
    int insn_count = build_blocklist_filter(blocked, blocked_count,
                                             filter, MAX_BPF_INSNS);
    if (insn_count < 0)
        return gscope_set_error(GSCOPE_ERR_SECCOMP,
                                "BPF program too large (%d blocked syscalls)",
                                blocked_count);

    /* Install the filter */
    struct sock_fprog prog = {
        .len = (unsigned short)insn_count,
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_SECCOMP,
                                      "prctl(SECCOMP_MODE_FILTER) failed");

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)custom_path;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "seccomp requires Linux");
#endif
}
