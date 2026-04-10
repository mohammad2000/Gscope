/*
 * test_security.c — Tests for security subsystem (seccomp, caps, priv)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/security.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  %-50s ", #name); } while(0)
#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); tests_passed++; } while(0)
#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m: %s\n", msg); tests_failed++; } while(0)
#define SKIP(msg) \
    do { printf("\033[33mSKIP\033[0m: %s\n", msg); tests_passed++; } while(0)

/* Forward declarations for internal functions */
extern gscope_err_t gscope_set_no_dumpable(void);
extern gscope_err_t gscope_clear_groups(gid_t primary_gid);
extern gscope_err_t gscope_drop_privileges(uid_t uid, gid_t gid);

static void test_caps_default_mask(void)
{
    TEST(security_caps_default_mask);

    uint64_t minimal = gscope_caps_default_mask(GSCOPE_ISOLATION_MINIMAL);
    uint64_t standard = gscope_caps_default_mask(GSCOPE_ISOLATION_STANDARD);
    uint64_t high = gscope_caps_default_mask(GSCOPE_ISOLATION_HIGH);
    uint64_t maximum = gscope_caps_default_mask(GSCOPE_ISOLATION_MAXIMUM);

    /* More isolation = fewer capabilities */
    if (minimal == 0) { FAIL("minimal should have caps"); return; }
    if (standard == 0) { FAIL("standard should have caps"); return; }
    if (high == 0) { FAIL("high should have caps"); return; }
    if (maximum == 0) { FAIL("maximum should have some caps"); return; }

    /* Verify ordering: minimal >= standard >= high >= maximum */
    /* Count bits */
    int count_min = __builtin_popcountll(minimal);
    int count_std = __builtin_popcountll(standard);
    int count_high = __builtin_popcountll(high);
    int count_max = __builtin_popcountll(maximum);

    if (count_min < count_std) { FAIL("minimal should have >= standard caps"); return; }
    if (count_std < count_high) { FAIL("standard should have >= high caps"); return; }
    if (count_high < count_max) { FAIL("high should have >= maximum caps"); return; }

    /* Standard should include GSCOPE_CAPS_DEFAULT */
    if ((standard & GSCOPE_CAPS_DEFAULT) != GSCOPE_CAPS_DEFAULT) {
        FAIL("standard should include GSCOPE_CAPS_DEFAULT");
        return;
    }

    PASS();
}

static void test_caps_constants(void)
{
    TEST(security_caps_constants);

    /* Verify capability bit positions match Linux definitions */
    if (GSCOPE_CAP_CHOWN != (1ULL << 0)) { FAIL("CAP_CHOWN wrong"); return; }
    if (GSCOPE_CAP_KILL != (1ULL << 5)) { FAIL("CAP_KILL wrong"); return; }
    if (GSCOPE_CAP_SETUID != (1ULL << 7)) { FAIL("CAP_SETUID wrong"); return; }
    if (GSCOPE_CAP_SETGID != (1ULL << 6)) { FAIL("CAP_SETGID wrong"); return; }
    if (GSCOPE_CAP_NET_BIND_SERVICE != (1ULL << 10)) { FAIL("CAP_NET_BIND_SERVICE wrong"); return; }
    if (GSCOPE_CAP_SYS_CHROOT != (1ULL << 18)) { FAIL("CAP_SYS_CHROOT wrong"); return; }

    PASS();
}

static void test_seccomp_disabled(void)
{
    TEST(security_seccomp_disabled);

    /* DISABLED profile should always succeed (no filter installed) */
    gscope_err_t err = gscope_seccomp_apply(GSCOPE_SECCOMP_DISABLED, NULL);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }

    PASS();
}

static void test_seccomp_custom_unsupported(void)
{
    TEST(security_seccomp_custom_unsupported);

#ifndef __linux__
    gscope_err_t err = gscope_seccomp_apply(GSCOPE_SECCOMP_DEFAULT, NULL);
    if (err != GSCOPE_ERR_UNSUPPORTED) { FAIL("should be UNSUPPORTED on non-Linux"); return; }
    PASS();
#else
    gscope_err_t err = gscope_seccomp_apply(GSCOPE_SECCOMP_CUSTOM, "/nonexistent");
    if (err != GSCOPE_ERR_UNSUPPORTED) { FAIL("custom should be UNSUPPORTED"); return; }
    PASS();
#endif
}

static void test_no_new_privs(void)
{
    TEST(security_no_new_privs);

#ifndef __linux__
    gscope_err_t err = gscope_no_new_privs();
    if (err != GSCOPE_ERR_UNSUPPORTED) { FAIL("should be UNSUPPORTED"); return; }
    PASS();
#else
    /*
     * NOTE: We CAN call this in the test because it only restricts
     * future exec'd processes, not the current one.
     * Once set, it cannot be unset (but that's fine for tests).
     */
    gscope_err_t err = gscope_no_new_privs();
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }
    PASS();
#endif
}

static void test_drop_privileges_root_noop(void)
{
    TEST(security_drop_privs_root_noop);

    /* Dropping to uid=0, gid=0 should be a no-op */
    gscope_err_t err = gscope_drop_privileges(0, 0);
    if (err != GSCOPE_OK) { FAIL("root noop should succeed"); return; }

    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — security\033[0m\n\n");

    test_caps_default_mask();
    test_caps_constants();
    test_seccomp_disabled();
    test_seccomp_custom_unsupported();
    test_no_new_privs();
    test_drop_privileges_root_noop();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
