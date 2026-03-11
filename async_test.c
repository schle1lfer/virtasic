/**
 * @file async_test.c
 * @brief Unit tests for the non-blocking async_func / async_worker subsystem.
 *
 * All tests use the polling model:
 *   1. async_func()          — submit, get request ID
 *   2. async_worker_status() — poll until ASYNC_STATUS_FREE
 *   3. async_get_result()    — copy result, release slot
 *
 * async_worker_wait() does not exist; there is no blocking wait path.
 *
 * Tests
 * -----
 *  1. test_single         – One request; poll then get result.
 *  2. test_multi_poll     – Four requests in flight simultaneously; poll all,
 *                           retrieve in reverse ID order.
 *  3. test_get_not_ready  – async_get_result() on an in-progress request must
 *                           return -1 (not ready).
 *  4. test_queue_full     – Fill all ASYNC_QUEUE_DEPTH slots; verify the next
 *                           submission returns ASYNC_REQ_INVALID; drain.
 *  5. test_sequential     – Five back-to-back requests to confirm slot reuse.
 */

#include "async_func.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    /* usleep */
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

/**
 * @brief Poll until the given request ID is no longer BUSY.
 *
 * Yields 500 µs between polls so the test thread does not spin-burn the CPU.
 *
 * @param[in] worker  Initialised worker handle.
 * @param[in] id      Request ID to wait on.
 */
static void poll_until_ready(async_worker_t *worker, async_req_id_t id)
{
    while (async_worker_status(worker, id) == ASYNC_STATUS_BUSY)
        usleep(500);
}

/* -------------------------------------------------------------------------
 * Functions dispatched asynchronously
 * ---------------------------------------------------------------------- */

/**
 * @brief Adds two int values from params[0] and params[1].
 *
 * @param[in]  params   params[0]: int A; params[1]: int B.
 * @param[in]  nparams  Must be 2.
 * @param[out] result   Receives int (A + B); iov_len set to sizeof(int).
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
 * @brief Computes strlen of the NUL-terminated string in params[0].
 *
 * @param[in]  params   params[0]: NUL-terminated string buffer.
 * @param[in]  nparams  Must be 1.
 * @param[out] result   Receives size_t length; iov_len set to sizeof(size_t).
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
 * @brief Sleeps 80 ms then adds two ints.  Holds the executor busy long
 *        enough to test BUSY status and premature async_get_result().
 *
 * @param[in]  params   params[0]: int A; params[1]: int B.
 * @param[in]  nparams  Must be 2.
 * @param[out] result   Receives int (A + B) after 80 ms.
 */
static void fn_slow_add(struct iovec *params, int nparams, struct iovec *result)
{
    usleep(80 * 1000);
    fn_add(params, nparams, result);
}

/**
 * @brief Sleeps 1 ms then adds two ints (used to saturate the queue quickly).
 *
 * @param[in]  params   params[0]: int A; params[1]: int B.
 * @param[in]  nparams  Must be 2.
 * @param[out] result   Receives int (A + B).
 */
static void fn_tiny_add(struct iovec *params, int nparams, struct iovec *result)
{
    usleep(1 * 1000);
    fn_add(params, nparams, result);
}

/* -------------------------------------------------------------------------
 * Test 1 – Single request: poll then retrieve
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 1 – Submit fn_add(7, 3), poll until FREE, retrieve result.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_single(async_worker_t *worker)
{
    printf("\n[Test 1] Single request: fn_add(7, 3) == 10\n");

    int a = 7, b = 3;
    struct iovec params[2] = {
        { .iov_base = &a, .iov_len = sizeof(a) },
        { .iov_base = &b, .iov_len = sizeof(b) },
    };

    async_req_id_t id = async_func(worker, fn_add, params, 2);
    ASSERT(id != ASYNC_REQ_INVALID, "async_func() returned valid request ID");
    printf("  Request ID: %u\n", id);

    /* Poll without blocking */
    poll_until_ready(worker, id);
    ASSERT(async_worker_status(worker, id) == ASYNC_STATUS_FREE,
           "status is FREE after polling");

    /* Retrieve result */
    int res = -1;
    struct iovec out = { .iov_base = &res, .iov_len = sizeof(res) };
    int rc = async_get_result(worker, id, &out);

    ASSERT(rc == 0,   "async_get_result() returned 0 (success)");
    ASSERT(res == 10, "result == 10");
    printf("  Result: %d\n", res);

    /* Slot released: status must now be FREE (not found) */
    ASSERT(async_worker_status(worker, id) == ASYNC_STATUS_FREE,
           "status FREE after slot released");

    /* Second call with the same ID must fail */
    int res2 = -1;
    struct iovec out2 = { .iov_base = &res2, .iov_len = sizeof(res2) };
    ASSERT(async_get_result(worker, id, &out2) == -1,
           "async_get_result() returns -1 for released ID");
}

/* -------------------------------------------------------------------------
 * Test 2 – Four requests in flight; collect in reverse order
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 2 – Four simultaneous requests, status-polled independently,
 *                 result retrieved in reverse submission order.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_multi_poll(async_worker_t *worker)
{
    printf("\n[Test 2] Four concurrent requests, retrieved in reverse order\n");

#define NR 4
    /* Request 0: fn_add(1, 1) → 2 */
    int a0 = 1, b0 = 1;
    struct iovec p0[2] = { { &a0, sizeof(a0) }, { &b0, sizeof(b0) } };

    /* Request 1: fn_strlen("hello") → 5 */
    const char *s1 = "hello";
    struct iovec p1[1] = { { (void *)s1, strlen(s1) + 1 } };

    /* Request 2: fn_add(4, 6) → 10 */
    int a2 = 4, b2 = 6;
    struct iovec p2[2] = { { &a2, sizeof(a2) }, { &b2, sizeof(b2) } };

    /* Request 3: fn_add(10, 20) → 30 */
    int a3 = 10, b3 = 20;
    struct iovec p3[2] = { { &a3, sizeof(a3) }, { &b3, sizeof(b3) } };

    async_req_id_t ids[NR];
    ids[0] = async_func(worker, fn_add,    p0, 2);
    ids[1] = async_func(worker, fn_strlen, p1, 1);
    ids[2] = async_func(worker, fn_add,    p2, 2);
    ids[3] = async_func(worker, fn_add,    p3, 2);

    for (int i = 0; i < NR; i++)
        ASSERT(ids[i] != ASYNC_REQ_INVALID, "submission accepted");

    /* Poll all four to completion */
    for (int i = 0; i < NR; i++)
        poll_until_ready(worker, ids[i]);

    /* Retrieve in reverse order */
    int     res0 = -1; struct iovec out0 = { &res0, sizeof(res0) };
    size_t  res1 =  0; struct iovec out1 = { &res1, sizeof(res1) };
    int     res2 = -1; struct iovec out2 = { &res2, sizeof(res2) };
    int     res3 = -1; struct iovec out3 = { &res3, sizeof(res3) };

    ASSERT(async_get_result(worker, ids[3], &out3) == 0, "get ids[3]");
    ASSERT(async_get_result(worker, ids[2], &out2) == 0, "get ids[2]");
    ASSERT(async_get_result(worker, ids[1], &out1) == 0, "get ids[1]");
    ASSERT(async_get_result(worker, ids[0], &out0) == 0, "get ids[0]");

    ASSERT(res3 == 30, "req3: 10+20 == 30");
    ASSERT(res2 == 10, "req2:  4+6  == 10");
    ASSERT(res1 ==  5, "req1: strlen(\"hello\") == 5");
    ASSERT(res0 ==  2, "req0:  1+1  == 2");

    printf("  Results: %d, %zu, %d, %d\n", res0, res1, res2, res3);
#undef NR
}

/* -------------------------------------------------------------------------
 * Test 3 – async_get_result() on an in-progress request returns -1
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 3 – Calling async_get_result() before the function finishes
 *                 must return -1 (not ready).
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_get_not_ready(async_worker_t *worker)
{
    printf("\n[Test 3] async_get_result() returns -1 while request is BUSY\n");

    int a = 3, b = 4;
    struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };

    async_req_id_t id = async_func(worker, fn_slow_add, params, 2);
    ASSERT(id != ASYNC_REQ_INVALID, "submission accepted");

    /* Spin until we observe BUSY (dispatcher may take a few µs) */
    int saw_busy = 0;
    for (int i = 0; i < 500 && !saw_busy; i++) {
        if (async_worker_status(worker, id) == ASYNC_STATUS_BUSY)
            saw_busy = 1;
        else
            usleep(1000);
    }
    ASSERT(saw_busy, "observed ASYNC_STATUS_BUSY while fn_slow_add runs");

    /* Premature result retrieval must fail */
    int res = -1;
    struct iovec out = { .iov_base = &res, .iov_len = sizeof(res) };
    int rc = async_get_result(worker, id, &out);
    ASSERT(rc == -1, "async_get_result() == -1 while still BUSY");
    ASSERT(res == -1, "result buffer untouched on -1 return");

    /* Poll until done, then collect */
    poll_until_ready(worker, id);
    rc = async_get_result(worker, id, &out);
    ASSERT(rc == 0,  "async_get_result() == 0 after becoming FREE");
    ASSERT(res == 7, "result == 7 (3 + 4)");
    printf("  Result: %d\n", res);
}

/* -------------------------------------------------------------------------
 * Test 4 – Queue-full: overflow returns ASYNC_REQ_INVALID
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 4 – Fill all ASYNC_QUEUE_DEPTH slots; the next submission must
 *                 be rejected with ASYNC_REQ_INVALID.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_queue_full(async_worker_t *worker)
{
    printf("\n[Test 4] Queue-full rejection (depth = %d)\n", ASYNC_QUEUE_DEPTH);

    async_req_id_t ids[ASYNC_QUEUE_DEPTH];
    int accepted = 0;

    int a = 1, b = 1;
    struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };

    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        ids[i] = async_func(worker, fn_tiny_add, params, 2);
        if (ids[i] != ASYNC_REQ_INVALID)
            accepted++;
    }
    printf("  Accepted %d / %d submissions\n", accepted, ASYNC_QUEUE_DEPTH);

    async_req_id_t overflow = async_func(worker, fn_add, params, 2);
    ASSERT(overflow == ASYNC_REQ_INVALID,
           "overflow submission rejected (ASYNC_REQ_INVALID)");
    ASSERT(accepted > 0, "at least one slot was accepted");

    /* Drain: poll each accepted request then retrieve its result */
    int dummy = -1;
    struct iovec out = { &dummy, sizeof(dummy) };
    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        if (ids[i] != ASYNC_REQ_INVALID) {
            poll_until_ready(worker, ids[i]);
            async_get_result(worker, ids[i], &out);
        }
    }
}

/* -------------------------------------------------------------------------
 * Test 5 – Sequential requests: verify slot reuse
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 5 – Five back-to-back submissions; each waits before the next.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_sequential(async_worker_t *worker)
{
    printf("\n[Test 5] Five sequential additions (slot reuse)\n");

    int all_pass = 1;
    for (int i = 0; i < 5; i++) {
        int a = i * 10, b = i;
        struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };

        async_req_id_t id = async_func(worker, fn_add, params, 2);
        if (id == ASYNC_REQ_INVALID) { all_pass = 0; break; }

        poll_until_ready(worker, id);

        int res = -1;
        struct iovec out = { &res, sizeof(res) };
        if (async_get_result(worker, id, &out) != 0) { all_pass = 0; break; }

        int expected = a + b;
        printf("  %2d + %d = %d  (expected %d)  ID=%u\n",
               a, b, res, expected, id);
        if (res != expected) all_pass = 0;
    }

    tests_run++;
    if (all_pass) {
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
 * Creates one async_worker_t, runs all five tests using the non-blocking
 * poll + async_get_result() pattern, destroys the handle, and prints a summary.
 *
 * @return EXIT_SUCCESS when all assertions pass, EXIT_FAILURE otherwise.
 */
int main(void)
{
    printf("=== async_func test suite (non-blocking, poll + get_result) ===\n");

    async_worker_t *worker = async_worker_init();
    if (!worker) {
        perror("async_worker_init");
        return EXIT_FAILURE;
    }

    test_single(worker);
    test_multi_poll(worker);
    test_get_not_ready(worker);
    test_queue_full(worker);
    test_sequential(worker);

    async_worker_destroy(worker);

    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
