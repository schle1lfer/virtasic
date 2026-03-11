/**
 * @file async_test.c
 * @brief Unit tests for the async_func / async_worker subsystem.
 *
 * Tests
 * -----
 *  1. test_add          – Submit a two-integer addition; verify the result.
 *  2. test_string_len   – Submit a strlen computation; verify the result.
 *  3. test_busy         – Submit a slow function, then immediately submit a
 *                         second request and verify ASYNC_STATUS_BUSY is returned.
 *  4. test_sequential   – Run five additions back-to-back to confirm the worker
 *                         correctly resets between requests.
 *  5. test_ack_before_exec – Verify that async_func() returns (async_worker ack)
 *                         before async_func_thread finishes execution, proving
 *                         the two-thread separation works correctly.
 */

#include "async_func.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    /* usleep */
#include <stdint.h>
#include <stdatomic.h> /* atomic_int */
#include <time.h>      /* clock_gettime */

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

/** Return monotonic time in milliseconds. */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

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

/*
 * Shared counter incremented by fn_slow_counter just before it returns.
 * Used by test_ack_before_exec to confirm that async_func() returned its
 * status BEFORE async_func_thread incremented the counter.
 */
static atomic_int g_exec_counter = 0;

/**
 * @brief Sleeps 80 ms then increments g_exec_counter.
 *
 * Used to verify that async_worker_thread sends its ack (via async_func())
 * before async_func_thread actually completes execution.
 *
 * @param[in]  params   Unused.
 * @param[in]  nparams  Unused.
 * @param[out] result   Sets iov_len = 0.
 */
static void fn_slow_counter(struct iovec *params, int nparams,
                             struct iovec *result)
{
    (void)params;
    (void)nparams;
    usleep(80 * 1000);    /* 80 ms */
    atomic_fetch_add(&g_exec_counter, 1);
    result->iov_len = 0;
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
 * @brief Test 3 – BUSY status when async_func_thread is occupied.
 *
 * Submits fn_slow_add (sleeps 100 ms), then immediately submits a second
 * request.  async_worker_thread must immediately return ASYNC_STATUS_BUSY
 * because async_func_thread is still running.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_busy(async_worker_t *worker)
{
    printf("\n[Test 3] BUSY rejection when async_func_thread is occupied\n");

    int a = 1, b = 2;
    struct iovec params[2] = {
        { .iov_base = &a, .iov_len = sizeof(a) },
        { .iov_base = &b, .iov_len = sizeof(b) },
    };

    int result_val = -1;
    struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };

    /* First submission: async_func_thread is free → accepted */
    async_status_t st1 = async_func(worker, fn_slow_add, params, 2, &result);
    ASSERT(st1 == ASYNC_STATUS_FREE, "first submission accepted (FREE)");

    /* Second submission: async_func_thread is busy → rejected */
    int result_val2 = -1;
    struct iovec result2 = { .iov_base = &result_val2,
                             .iov_len  = sizeof(result_val2) };
    async_status_t st2 = async_func(worker, fn_add, params, 2, &result2);
    ASSERT(st2 == ASYNC_STATUS_BUSY, "second submission rejected (BUSY)");

    async_worker_wait(worker);
    ASSERT(result_val == 3, "slow_add result == 3 after wait");

    printf("  slow_add result: %d\n", result_val);
}

/**
 * @brief Test 4 – Sequential submissions on the same worker.
 *
 * Runs five additions back-to-back to verify both threads correctly reset
 * between jobs and always produce the right answer.
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

/**
 * @brief Test 5 – async_worker acknowledges before async_func_thread finishes.
 *
 * Submits fn_slow_counter (sleeps 80 ms then increments g_exec_counter).
 * Measures the time async_func() takes to return and checks that:
 *  a) async_func() returned much faster than 80 ms (async_worker acked first).
 *  b) g_exec_counter is still 0 immediately after async_func() returns,
 *     proving async_func_thread had not yet finished (and possibly not started).
 *  c) After async_worker_wait() g_exec_counter == 1.
 *
 * This directly validates the two-thread separation: async_worker_thread posts
 * ASYNC_STATUS_FREE to the caller before async_func_thread executes anything.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_ack_before_exec(async_worker_t *worker)
{
    printf("\n[Test 5] async_worker acks before async_func_thread finishes\n");

    atomic_store(&g_exec_counter, 0);

    struct iovec result = { .iov_base = NULL, .iov_len = 0 };

    long t0 = now_ms();
    async_status_t st = async_func(worker, fn_slow_counter, NULL, 0, &result);
    long elapsed = now_ms() - t0;

    ASSERT(st == ASYNC_STATUS_FREE, "async_func returned FREE");

    /* async_func() must have returned well before the 80 ms sleep in
     * fn_slow_counter ends.  Allow 50 ms of scheduling slack.          */
    printf("  async_func() returned in %ld ms (function sleeps 80 ms)\n",
           elapsed);
    ASSERT(elapsed < 50,
           "async_func() returned before async_func_thread finished (< 50 ms)");

    /* g_exec_counter must still be 0: async_func_thread has not yet
     * completed (and may not have started its 80 ms sleep yet).        */
    int counter_after_submit = atomic_load(&g_exec_counter);
    printf("  g_exec_counter immediately after async_func(): %d\n",
           counter_after_submit);
    ASSERT(counter_after_submit == 0,
           "execution not yet complete when async_func() returned");

    /* Now wait for async_func_thread to finish */
    async_worker_wait(worker);

    int counter_after_wait = atomic_load(&g_exec_counter);
    printf("  g_exec_counter after async_worker_wait(): %d\n",
           counter_after_wait);
    ASSERT(counter_after_wait == 1,
           "g_exec_counter == 1 after async_worker_wait()");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

/**
 * @brief Test harness entry point.
 *
 * Creates one async_worker_t (two internal threads), runs all tests, destroys
 * it, then prints a summary.
 *
 * @return EXIT_SUCCESS when all tests pass, EXIT_FAILURE otherwise.
 */
int main(void)
{
    printf("=== async_func test suite (two-thread design) ===\n");

    async_worker_t *worker = async_worker_init();
    if (!worker) {
        perror("async_worker_init");
        return EXIT_FAILURE;
    }

    test_add(worker);
    test_string_len(worker);
    test_busy(worker);
    test_sequential(worker);
    test_ack_before_exec(worker);

    async_worker_destroy(worker);

    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
