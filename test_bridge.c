/**
 * @file test_bridge.c
 * @brief Integration test for bridge_create() and bridge_remove().
 *
 * The test is structured in two parts:
 *
 *  Part A – Input validation (no kernel interaction required)
 *    A1: bridge_create(NULL)   → expects -EINVAL (null MAC)
 *    A2: bridge_create("bad")  → expects -EINVAL (invalid MAC string)
 *
 *  Part B – Full bridge lifecycle (requires CAP_NET_ADMIN + bridge/dummy modules)
 *    B1: bridge_create("aa:bb:cc:dd:ee:ff") → creates Bridge + dummy
 *    B2: bridge_create("aa:bb:cc:dd:ee:ff") → expects -EEXIST (Bridge present)
 *    B3: bridge_remove()                    → deletes Bridge + dummy
 *    B4: bridge_remove()                    → expects 0 (idempotent when absent)
 *
 * All [NETLINK] timing lines produced by NL_CALL_* macros are printed
 * regardless of pass/fail so that the complete Netlink call sequence is
 * visible for analysis.
 *
 * Run as root (or with CAP_NET_ADMIN) so that RTM_NEWLINK / RTM_SETLINK /
 * RTM_DELLINK messages reach the kernel.  In restricted environments that
 * lack the bridge or dummy kernel module, Part B steps will fail and the
 * test will report the failures; Part A validation steps will still pass.
 */

#include <errno.h>
#include <stdio.h>

#include "vlan_api.h"
#include "bridge_api.h"

#define TEST_MAC         "aa:bb:cc:dd:ee:ff"
#define TEST_INVALID_MAC "not-a-mac"

/* -------------------------------------------------------------------------
 * Test helpers
 * ------------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

static void print_separator(void)
{
    printf("------------------------------------------------------------\n");
}

/**
 * check() - Verify that a function returned the expected code.
 *
 * @param desc      Human-readable step description.
 * @param got       Actual return value.
 * @param expected  Expected return value (0 = success, negative = error code).
 */
static void check(const char *desc, int got, int expected)
{
    if (got == expected)
    {
        printf("[PASS] %s  (ret=%d)\n", desc, got);
        g_pass++;
    }
    else
    {
        printf("[FAIL] %s  (expected=%d, got=%d)\n", desc, expected, got);
        g_fail++;
    }
}

/* -------------------------------------------------------------------------
 * Part A – Input validation
 * ------------------------------------------------------------------------- */

static void test_validation(void)
{
    int ret;

    printf("\n");
    printf("============================================================\n");
    printf("  Part A: Input validation\n");
    printf("============================================================\n\n");

    /* ------------------------------------------------------------------
     * A1: bridge_create(NULL) – must reject null MAC pointer.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("A1: bridge_create(NULL)  →  expect -EINVAL\n");
    print_separator();
    ret = bridge_create(NULL);
    check("bridge_create(NULL)", ret, -EINVAL);
    printf("\n");

    /* ------------------------------------------------------------------
     * A2: bridge_create("not-a-mac") – must reject unparseable MAC string.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("A2: bridge_create(\"%s\")  →  expect -EINVAL\n", TEST_INVALID_MAC);
    print_separator();
    ret = bridge_create(TEST_INVALID_MAC);
    check("bridge_create(invalid MAC)", ret, -EINVAL);
    printf("\n");
}

/* -------------------------------------------------------------------------
 * Part B – Full bridge lifecycle
 * ------------------------------------------------------------------------- */

static void test_lifecycle(void)
{
    int ret;

    printf("============================================================\n");
    printf("  Part B: Full bridge lifecycle  (MAC \"%s\")\n", TEST_MAC);
    printf("============================================================\n\n");

    /* ------------------------------------------------------------------
     * B1: bridge_create(TEST_MAC)
     * Creates Bridge (type bridge, IFF_UP, MTU 9100, mac set, vlan_filtering=1)
     * and dummy (type dummy, enslaved to Bridge), VLAN 1 removed from Bridge.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("B1: bridge_create(\"%s\")\n", TEST_MAC);
    print_separator();
    ret = bridge_create(TEST_MAC);
    check("bridge_create(\"" TEST_MAC "\")", ret, 0);
    printf("\n");

    /* ------------------------------------------------------------------
     * B2: bridge_create(TEST_MAC) – Bridge already exists, must return -EEXIST.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("B2: bridge_create(\"%s\")  →  expect -EEXIST (already present)\n",
           TEST_MAC);
    print_separator();
    ret = bridge_create(TEST_MAC);
    check("bridge_create(duplicate)", ret, -EEXIST);
    printf("\n");

    /* ------------------------------------------------------------------
     * B3: bridge_remove()
     * Deletes Bridge and dummy interfaces.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("B3: bridge_remove()\n");
    print_separator();
    ret = bridge_remove();
    check("bridge_remove()", ret, 0);
    printf("\n");

    /* ------------------------------------------------------------------
     * B4: bridge_remove() – interfaces are gone, must still return 0.
     * ------------------------------------------------------------------ */
    print_separator();
    printf("B4: bridge_remove()  →  expect 0 (idempotent, interfaces absent)\n");
    print_separator();
    ret = bridge_remove();
    check("bridge_remove(idempotent)", ret, 0);
    printf("\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* Flush [NETLINK] lines immediately even when stdout is redirected. */
    setbuf(stdout, NULL);

    printf("============================================================\n");
    printf("  virtasic bridge API test\n");
    printf("============================================================\n");

    test_validation();
    test_lifecycle();

    printf("============================================================\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("============================================================\n");

    return (g_fail > 0) ? 1 : 0;
}
