/*
 * fs/mount.c — Mount /proc, /sys, /dev inside scope rootfs
 *
 * These mounts are done AFTER pivot_root, inside the new mount namespace.
 * They provide the scope with its own view of:
 *   /proc  — process information (PID namespace aware)
 *   /sys   — sysfs (read-only)
 *   /dev/pts — pseudo-terminals
 *   /dev/shm — shared memory
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/mount.h>
#include <sys/sysmacros.h>   /* makedev(), major(), minor() */
#endif

/* ─── OCI-required device nodes ──────────────────────────────────── */

struct dev_node {
    const char *path;
    mode_t      mode;     /* S_IFCHR, S_IFBLK */
    int         major;
    int         minor;
    mode_t      perms;
};

static const struct dev_node required_devices[] = {
    { "dev/null",    S_IFCHR, 1, 3, 0666 },
    { "dev/zero",    S_IFCHR, 1, 5, 0666 },
    { "dev/full",    S_IFCHR, 1, 7, 0666 },
    { "dev/random",  S_IFCHR, 1, 8, 0666 },
    { "dev/urandom", S_IFCHR, 1, 9, 0666 },
    { "dev/tty",     S_IFCHR, 5, 0, 0666 },
    { NULL, 0, 0, 0, 0 }
};

/* Paths to mask (bind-mount /dev/null over them) */
static const char *masked_paths[] = {
    "proc/asound",
    "proc/acpi",
    "proc/kcore",
    "proc/keys",
    "proc/latency_stats",
    "proc/timer_list",
    "proc/timer_stats",
    "proc/sched_debug",
    "proc/scsi",
    "sys/firmware",
    "sys/devices/virtual/powercap",
    NULL
};

/* Paths to make read-only (bind-mount RO) */
static const char *readonly_paths[] = {
    "proc/bus",
    "proc/fs",
    "proc/irq",
    "proc/sys",
    "proc/sysrq-trigger",
    NULL
};

/* ─── Mount helpers ──────────────────────────────────────────────── */

#ifdef __linux__

/*
 * Mount a filesystem inside the rootfs.
 * Returns 0 on success, -1 on failure (sets errno).
 */
static int do_mount(const char *rootfs, const char *target,
                    const char *type, unsigned long flags,
                    const char *data)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", rootfs, target);

    /* Ensure mount point exists */
    gscope_mkdir_p(path, 0755);

    return mount(type, path, type, flags, data);
}

#endif /* __linux__ */

/* ─── Public API ─────────────────────────────────────────────────── */

/*
 * Mount essential filesystems inside the scope rootfs.
 * Called from the child process after pivot_root.
 *
 * rootfs: the new root (e.g. "/opt/gritiva/scopes/42/rootfs")
 *         or "/" if already pivoted.
 */
gscope_err_t gscope_mount_essential(const char *rootfs)
{
    if (!rootfs)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL rootfs");

#ifdef __linux__
    int errors = 0;

    /* /proc — process information filesystem */
    if (do_mount(rootfs, "proc", "proc",
                 MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        /* proc mount might fail if already mounted (nested) — non-fatal */
        if (errno != EBUSY)
            errors++;
    }

    /* /sys — sysfs (read-only for safety) */
    if (do_mount(rootfs, "sys", "sysfs",
                 MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL) != 0) {
        if (errno != EBUSY)
            errors++;
    }

    /* /dev/pts — pseudo-terminal slave devices */
    {
        char pts[4096];
        snprintf(pts, sizeof(pts), "%s/dev/pts", rootfs);
        gscope_mkdir_p(pts, 0755);

        if (mount("devpts", pts, "devpts",
                  MS_NOSUID | MS_NOEXEC,
                  "newinstance,ptmxmode=0666,mode=0620") != 0) {
            if (errno != EBUSY)
                errors++;
        }
    }

    /* /dev/shm — shared memory (tmpfs) */
    {
        char shm[4096];
        snprintf(shm, sizeof(shm), "%s/dev/shm", rootfs);
        gscope_mkdir_p(shm, 0755);

        if (mount("shm", shm, "tmpfs",
                  MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  "size=64m,mode=1777") != 0) {
            if (errno != EBUSY)
                errors++;
        }
    }

    /* /tmp — tmpfs (optional, useful if overlay doesn't cover it) */
    {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s/tmp", rootfs);
        gscope_mkdir_p(tmp, 01777);

        /* Only mount if /tmp isn't already present from overlay */
        struct stat st;
        if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) {
            mount("tmpfs", tmp, "tmpfs",
                  MS_NOSUID | MS_NODEV,
                  "size=256m,mode=1777");
            /* Non-fatal if fails */
        }
    }

    if (errors > 0)
        return gscope_set_error(GSCOPE_ERR_MOUNT,
                                "failed to mount %d essential filesystem(s)",
                                errors);
#else
    (void)rootfs;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "mount requires Linux");
#endif

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Dev Setup (OCI device allowlist) ────────────────────────────── */

/*
 * Create required /dev device nodes inside the scope rootfs.
 *
 * OCI Runtime Spec mandates these devices exist:
 *   /dev/null, /dev/zero, /dev/full, /dev/random, /dev/urandom, /dev/tty
 *
 * Also creates symlinks:
 *   /dev/ptmx  → /dev/pts/ptmx
 *   /dev/fd    → /proc/self/fd
 *   /dev/stdin → /proc/self/fd/0
 *   /dev/stdout→ /proc/self/fd/1
 *   /dev/stderr→ /proc/self/fd/2
 */
gscope_err_t gscope_dev_setup(const char *rootfs)
{
    if (!rootfs)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL rootfs");

#ifdef __linux__
    char path[4096];

    /* Ensure /dev exists */
    snprintf(path, sizeof(path), "%s/dev", rootfs);
    gscope_mkdir_p(path, 0755);

    /* Create device nodes */
    for (int i = 0; required_devices[i].path; i++) {
        snprintf(path, sizeof(path), "%s/%s", rootfs, required_devices[i].path);

        /* Skip if already exists (from template) */
        struct stat st;
        if (stat(path, &st) == 0)
            continue;

        dev_t dev = makedev(required_devices[i].major, required_devices[i].minor);
        if (mknod(path, required_devices[i].mode | required_devices[i].perms, dev) != 0) {
            /* Non-fatal: might lack permissions in user namespace */
            continue;
        }
        chmod(path, required_devices[i].perms);
    }

    /* Create symlinks */
    struct { const char *link; const char *target; } symlinks[] = {
        { "dev/ptmx",   "/dev/pts/ptmx"   },
        { "dev/fd",     "/proc/self/fd"    },
        { "dev/stdin",  "/proc/self/fd/0"  },
        { "dev/stdout", "/proc/self/fd/1"  },
        { "dev/stderr", "/proc/self/fd/2"  },
        { NULL, NULL }
    };

    for (int i = 0; symlinks[i].link; i++) {
        snprintf(path, sizeof(path), "%s/%s", rootfs, symlinks[i].link);
        /* Remove existing (might be a file from template) */
        unlink(path);
        symlink(symlinks[i].target, path);
    }

#else
    (void)rootfs;
#endif

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Mask Sensitive Paths ───────────────────────────────────────── */

/*
 * Mask sensitive kernel paths inside the scope.
 *
 * Masked paths: bind-mount /dev/null over them (unreadable)
 * Read-only paths: remount as read-only (can read, cannot write)
 *
 * This prevents information leaks (kernel keys, timing data,
 * scheduling debug info) and configuration changes (/proc/sys).
 *
 * Must be called AFTER mount_essential and AFTER pivot_root.
 */
gscope_err_t gscope_mask_paths(const char *rootfs)
{
    if (!rootfs)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL rootfs");

#ifdef __linux__
    char path[4096];
    char null_path[4096];
    snprintf(null_path, sizeof(null_path), "%s/dev/null", rootfs);

    /* Mask paths: bind-mount /dev/null over them */
    for (int i = 0; masked_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", rootfs, masked_paths[i]);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;  /* Path doesn't exist — skip */

        if (S_ISDIR(st.st_mode)) {
            /* For directories: mount tmpfs (empty, read-only) */
            mount("tmpfs", path, "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=0");
        } else {
            /* For files: bind-mount /dev/null */
            mount(null_path, path, NULL, MS_BIND, NULL);
        }
    }

    /* Read-only paths: bind-mount then remount RO */
    for (int i = 0; readonly_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", rootfs, readonly_paths[i]);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        /* First bind-mount onto itself */
        if (mount(path, path, NULL, MS_BIND | MS_REC, NULL) != 0)
            continue;

        /* Then remount as read-only */
        mount(path, path, NULL, MS_BIND | MS_REC | MS_RDONLY | MS_REMOUNT, NULL);
    }

#else
    (void)rootfs;
#endif

    gscope_clear_error();
    return GSCOPE_OK;
}

/*
 * Unmount essential filesystems (reverse order).
 * Called during scope cleanup.
 */
gscope_err_t gscope_unmount_essential(const char *rootfs)
{
    if (!rootfs)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL rootfs");

#ifdef __linux__
    const char *mounts[] = {
        "tmp", "dev/shm", "dev/pts", "sys", "proc", NULL
    };

    for (int i = 0; mounts[i]; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", rootfs, mounts[i]);

        /* Try normal unmount, then lazy if busy */
        if (umount(path) != 0) {
            if (errno == EBUSY)
                umount2(path, MNT_DETACH);
            /* Ignore EINVAL (not mounted) and ENOENT */
        }
    }
#else
    (void)rootfs;
#endif

    gscope_clear_error();
    return GSCOPE_OK;
}
