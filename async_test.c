/**
 * @file async_test.c
 * @brief Unit tests for the async_func / async_worker subsystem.
 *
 * Tests
 * -----
 *  1. test_add        – Submit a two-integer addition; verify the result.
 *  2. test_string_len – Submit a strlen computation; verify the result.
 *  3. test_busy       – Submit a slow function, then immediately submit a
 *                       second request and verify ASYNC_STATUS_BUSY is returned.
 *  4. test_sequential – Run five additions back-to-back on the same worker to
 *                       confirm the worker correctly resets between requests.
 */

#include "async_func.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* usleep */
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Test helpers
 * ---------------------------------------------------------------------- */

static int tests_run    = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg)                                                    \
    do {                                                                     \
        tests_run++;                                                         \
        if (cond) {                                                          \
            tests_passed++;                                                  \
            printf("  [PASS] %s\n", (msg));                                  \
        } else {                                                             \
            printf("  [FAIL] %s  (line %d)\n", (msg), __LINE__);            \
        }                                                                    \
    } while (0)

/* -------------------------------------------------------------------------
 * Functions dispatched asynchronously
 * ---------------------------------------------------------------------- */

/**
 * @brief Adds two int values passed via params[0] and params[1].
 *
 * @param[in]  params   params[0]: int addend A; params[1]: int addend B.
 * @param[in]  nparams  Must be 2.
 * @param[out] result   Receives an int holding A + B; iov_len set to sizeof(int).
 */
static void fn_add(struct iovec *params, int nparams, struct iovec *result)
{
    if (nparams < 2 || result->iov_len < sizeof(int))
        return;

    int a, b;
    memcpy(&a, params[0].iov_base, sizeof(int));
    memcpy(&b, params[1].iov_base, sizeof(int));

    int sum = a + b;
    memcpy(result->iov_base, &sum, sizeof(int));
    result->iov_len = sizeof(int);
}

/**
 * @brief Computes strlen of the string in params[0].
 *
 * @param[in]  params   params[0]: NUL-terminated string.
 * @param[in]  nparams  Must be 1.
 * @param[out] result   Receives a size_t with the string length;
 *                      iov_len set to sizeof(size_t).
 */
static void fn_strlen(struct iovec *params, int nparams, struct iovec *result)
{
    if (nparams < 1 || result->iov_len < sizeof(size_t))
        return;

    size_t len = strlen((const char *)params[0].iov_base);
    memcpy(result->iov_base, &len, sizeof(size_t));
    result->iov_len = sizeof(size_t);
}

/**
 * @brief Simulates a slow function by sleeping 100 ms, then adds two ints.
 *
 * Used to test ASYNC_STATUS_BUSY: submit this, then immediately try to submit
 * a second request before this one finishes.
 *
 * @param[in]  params   params[0]: int A; params[1]: int B.
 * @param[in]  nparams  Must be 2.
 * @param[out] result   Receives int A + B after a 100 ms delay.
 */
static void fn_slow_add(struct iovec *params, int nparams, struct iovec *result)
{
    usleep(100 * 1000);   /* 100 ms */
    fn_add(params, nparams, result);
}

/* -------------------------------------------------------------------------
 * Individual tests
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 1 – Basic integer addition.
 *
 * Submits fn_add(7, 3), waits for completion, checks result == 10.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_add(async_worker_t *worker)
{
    printf("\n[Test 1] Basic integer addition (7 + 3 = 10)\n");

    int a = 7, b = 3;
    struct iovec params[2] = {
        { .iov_base = &a, .iov_len = sizeof(a) },
        { .iov_base = &b, .iov_len = sizeof(b) },
    };

    int result_val = -1;
    struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };

    async_status_t st = async_func(worker, fn_add, params, 2, &result);
    ASSERT(st == ASYNC_STATUS_FREE, "async_func returned FREE (request accepted)");

    async_worker_wait(worker);
    ASSERT(result_val == 10, "result == 10");

    printf("  Result: %d\n", result_val);
}

/**
 * @brief Test 2 – String length computation.
 *
 * Submits fn_strlen("Hello, world!"), waits for completion, checks result == 13.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_string_len(async_worker_t *worker)
{
    printf("\n[Test 2] String length (\"Hello, world!\" -> 13)\n");

    const char *str = "Hello, world!";
    struct iovec params[1] = {
        { .iov_base = (void *)str, .iov_len = strlen(str) + 1 },
    };

    size_t result_val = 0;
    struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };

    async_status_t st = async_func(worker, fn_strlen, params, 1, &result);
    ASSERT(st == ASYNC_STATUS_FREE, "async_func returned FREE (request accepted)");

    async_worker_wait(worker);
    ASSERT(result_val == 13, "result == 13");

    printf("  Result: %zu\n", result_val);
}

/**
 * @brief Test 3 – BUSY status when worker is occupied.
 *
 * Submits fn_slow_add (sleeps 100 ms), then immediately submits a second
 * request.  The second submission must be rejected with ASYNC_STATUS_BUSY.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_busy(async_worker_t *worker)
{
    printf("\n[Test 3] BUSY rejection when worker is occupied\n");

    int a = 1, b = 2;
    struct iovec params[2] = {
        { .iov_base = &a, .iov_len = sizeof(a) },
        { .iov_base = &b, .iov_len = sizeof(b) },
    };

    int result_val = -1;
    struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };

    /* First submission: accepted */
    async_status_t st1 = async_func(worker, fn_slow_add, params, 2, &result);
    ASSERT(st1 == ASYNC_STATUS_FREE, "first submission accepted (FREE)");

    /* Second submission while worker is sleeping: must be rejected */
    int result_val2 = -1;
    struct iovec result2 = { .iov_base = &result_val2, .iov_len = sizeof(result_val2) };
    async_status_t st2 = async_func(worker, fn_add, params, 2, &result2);
    ASSERT(st2 == ASYNC_STATUS_BUSY, "second submission rejected (BUSY)");

    /* Wait for slow function to finish */
    async_worker_wait(worker);
    ASSERT(result_val == 3, "slow_add result == 3 after wait");

    printf("  slow_add result: %d\n", result_val);
}

/**
 * @brief Test 4 – Sequential submissions on the same worker.
 *
 * Runs five additions back-to-back to verify the worker correctly resets
 * between jobs and always produces the right answer.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_sequential(async_worker_t *worker)
{
    printf("\n[Test 4] Five sequential additions\n");

    int pass = 1;
    for (int i = 0; i < 5; i++) {
        int a = i * 10, b = i;
        struct iovec params[2] = {
            { .iov_base = &a, .iov_len = sizeof(a) },
            { .iov_base = &b, .iov_len = sizeof(b) },
        };

        int result_val = -1;
        struct iovec result = { .iov_base = &result_val,
                                .iov_len  = sizeof(result_val) };

        async_status_t st = async_func(worker, fn_add, params, 2, &result);
        if (st != ASYNC_STATUS_FREE) { pass = 0; break; }

        async_worker_wait(worker);

        int expected = a + b;
        printf("  %d + %d = %d  (expected %d)\n", a, b, result_val, expected);
        if (result_val != expected) { pass = 0; }
    }

    tests_run++;
    if (pass) {
        tests_passed++;
        printf("  [PASS] all five sequential additions correct\n");
    } else {
        printf("  [FAIL] one or more sequential additions wrong  (line %d)\n",
               __LINE__);
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

/**
 * @brief Test harness entry point.
 *
 * Creates one async_worker_t, runs all tests, destroys it, then prints a
 * summary.
 *
 * @return EXIT_SUCCESS when all tests pass, EXIT_FAILURE otherwise.
 */
int main(void)
{
    printf("=== async_func test suite ===\n");

    async_worker_t *worker = async_worker_init();
    if (!worker) {
        perror("async_worker_init");
        return EXIT_FAILURE;
    }

    test_add(worker);
    test_string_len(worker);
    test_busy(worker);
    test_sequential(worker);

    async_worker_destroy(worker);

    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
