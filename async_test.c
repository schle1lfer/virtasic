/**
 * @file async_test.c
 * @brief Unit tests for the queue-based async_func / async_worker subsystem.
 *
 * Tests
 * -----
 *  1. test_single       – Submit one request, wait by ID, verify result.
 *  2. test_queue_fifo   – Submit four requests without waiting; collect them
 *                         in reverse order to confirm per-ID tracking works.
 *  3. test_status       – Submit a slow function; poll async_worker_status()
 *                         while it runs (must be BUSY), then after
 *                         async_worker_wait() it must be FREE.
 *  4. test_queue_full   – Fill all ASYNC_QUEUE_DEPTH slots, verify the next
 *                         submission returns ASYNC_REQ_INVALID, then drain.
 *  5. test_sequential   – Submit five additions back-to-back (wait between
 *                         each) to confirm slot reuse works correctly.
 */

#include "async_func.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    /* usleep */
#include <stdint.h>
#include <stdatomic.h>

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
 * @brief Sleeps 80 ms then adds two ints — used to hold the executor busy.
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
 * @brief Sleeps 1 ms then adds two ints — used to fill the queue quickly.
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
 * Test 1 – Single request, wait by ID
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 1 – Basic single submission and per-ID wait.
 *
 * Submits fn_add(7, 3), waits by request ID, verifies result == 10.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_single(async_worker_t *worker)
{
    printf("\n[Test 1] Single request: fn_add(7, 3) == 10\n");

    int a = 7, b = 3, res = -1;
    struct iovec params[2] = {
        { .iov_base = &a, .iov_len = sizeof(a) },
        { .iov_base = &b, .iov_len = sizeof(b) },
    };
    struct iovec result = { .iov_base = &res, .iov_len = sizeof(res) };

    async_req_id_t id = async_func(worker, fn_add, params, 2, &result);
    ASSERT(id != ASYNC_REQ_INVALID, "async_func() returned valid request ID");
    printf("  Request ID: %u\n", id);

    async_worker_wait(worker, id);
    ASSERT(res == 10, "result == 10");
    printf("  Result: %d\n", res);
}

/* -------------------------------------------------------------------------
 * Test 2 – FIFO queue: submit four, collect in reverse order
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 2 – Four queued requests collected out of submission order.
 *
 * Submits fn_add(1,1), fn_strlen("hello"), fn_add(4,6), fn_add(10,20),
 * then waits for them in reverse order by request ID.  Verifies that
 * per-ID tracking correctly maps each result to its original request.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_queue_fifo(async_worker_t *worker)
{
    printf("\n[Test 2] Four queued requests collected in reverse order\n");

#define NREQ 4
    async_req_id_t ids[NREQ];

    /* Request 0: fn_add(1, 1) → 2 */
    int a0 = 1, b0 = 1, res0 = -1;
    struct iovec p0[2] = { { &a0, sizeof(a0) }, { &b0, sizeof(b0) } };
    struct iovec r0 = { &res0, sizeof(res0) };

    /* Request 1: fn_strlen("hello") → 5 */
    const char *s1 = "hello";
    size_t res1 = 0;
    struct iovec p1[1] = { { (void *)s1, strlen(s1) + 1 } };
    struct iovec r1 = { &res1, sizeof(res1) };

    /* Request 2: fn_add(4, 6) → 10 */
    int a2 = 4, b2 = 6, res2 = -1;
    struct iovec p2[2] = { { &a2, sizeof(a2) }, { &b2, sizeof(b2) } };
    struct iovec r2 = { &res2, sizeof(res2) };

    /* Request 3: fn_add(10, 20) → 30 */
    int a3 = 10, b3 = 20, res3 = -1;
    struct iovec p3[2] = { { &a3, sizeof(a3) }, { &b3, sizeof(b3) } };
    struct iovec r3 = { &res3, sizeof(res3) };

    ids[0] = async_func(worker, fn_add,    p0, 2, &r0);
    ids[1] = async_func(worker, fn_strlen, p1, 1, &r1);
    ids[2] = async_func(worker, fn_add,    p2, 2, &r2);
    ids[3] = async_func(worker, fn_add,    p3, 2, &r3);

    for (int i = 0; i < NREQ; i++)
        ASSERT(ids[i] != ASYNC_REQ_INVALID, "submission accepted");

    /* Collect in reverse order */
    async_worker_wait(worker, ids[3]); ASSERT(res3 == 30, "req3: 10+20 == 30");
    async_worker_wait(worker, ids[2]); ASSERT(res2 == 10, "req2:  4+6  == 10");
    async_worker_wait(worker, ids[1]); ASSERT(res1 ==  5, "req1: strlen(hello) == 5");
    async_worker_wait(worker, ids[0]); ASSERT(res0 ==  2, "req0:  1+1  == 2");

    printf("  Results: %d, %zu, %d, %d\n", res0, res1, res2, res3);
#undef NREQ
}

/* -------------------------------------------------------------------------
 * Test 3 – Status polling: BUSY while running, FREE after wait
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 3 – async_worker_status() returns BUSY then FREE.
 *
 * Submits fn_slow_add (80 ms).  Polls status in a loop until BUSY is
 * observed, then waits and checks FREE.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_status(async_worker_t *worker)
{
    printf("\n[Test 3] Status: BUSY while running, FREE after wait\n");

    int a = 5, b = 7, res = -1;
    struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };
    struct iovec result = { &res, sizeof(res) };

    async_req_id_t id = async_func(worker, fn_slow_add, params, 2, &result);
    ASSERT(id != ASYNC_REQ_INVALID, "submission accepted");

    /* Poll until we catch the request as BUSY (may need a few iterations
     * before the dispatcher picks it up).                                 */
    int saw_busy = 0;
    for (int i = 0; i < 200; i++) {
        if (async_worker_status(worker, id) == ASYNC_STATUS_BUSY) {
            saw_busy = 1;
            break;
        }
        usleep(1000);
    }
    ASSERT(saw_busy, "observed ASYNC_STATUS_BUSY while request is in progress");

    async_worker_wait(worker, id);

    async_status_t st_after = async_worker_status(worker, id);
    ASSERT(st_after == ASYNC_STATUS_FREE,
           "status is FREE (slot released) after async_worker_wait()");
    ASSERT(res == 12, "result == 12 (5 + 7)");
    printf("  Result: %d\n", res);
}

/* -------------------------------------------------------------------------
 * Test 4 – Queue-full rejection and drain
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 4 – Filling all queue slots triggers ASYNC_REQ_INVALID.
 *
 * Submits ASYNC_QUEUE_DEPTH slow requests to saturate the pool, then verifies
 * that one more submission is rejected.  Drains all requests afterwards to
 * leave the worker clean for subsequent tests.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_queue_full(async_worker_t *worker)
{
    printf("\n[Test 4] Queue-full rejection (depth = %d)\n", ASYNC_QUEUE_DEPTH);

    async_req_id_t ids[ASYNC_QUEUE_DEPTH];
    int results[ASYNC_QUEUE_DEPTH];
    struct iovec result_vecs[ASYNC_QUEUE_DEPTH];

    int a = 1, b = 1;
    struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };

    int accepted = 0;
    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        results[i] = -1;
        result_vecs[i].iov_base = &results[i];
        result_vecs[i].iov_len  = sizeof(results[i]);
        ids[i] = async_func(worker, fn_tiny_add, params, 2, &result_vecs[i]);
        if (ids[i] != ASYNC_REQ_INVALID)
            accepted++;
    }
    printf("  Accepted %d / %d submissions\n", accepted, ASYNC_QUEUE_DEPTH);

    /* One extra submission must now be rejected */
    int extra_res = -1;
    struct iovec extra_result = { &extra_res, sizeof(extra_res) };
    async_req_id_t overflow_id = async_func(worker, fn_add, params, 2,
                                            &extra_result);
    ASSERT(overflow_id == ASYNC_REQ_INVALID,
           "overflow submission rejected (ASYNC_REQ_INVALID)");

    /* Drain all accepted requests */
    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        if (ids[i] != ASYNC_REQ_INVALID)
            async_worker_wait(worker, ids[i]);
    }
    ASSERT(accepted > 0, "at least one slot was accepted before queue full");
}

/* -------------------------------------------------------------------------
 * Test 5 – Sequential reuse of slots
 * ---------------------------------------------------------------------- */

/**
 * @brief Test 5 – Five sequential submissions (wait between each).
 *
 * Verifies that slots are correctly recycled and that each result is
 * independently correct.
 *
 * @param[in] worker  Initialised worker handle.
 */
static void test_sequential(async_worker_t *worker)
{
    printf("\n[Test 5] Five sequential additions (slot reuse)\n");

    int all_pass = 1;
    for (int i = 0; i < 5; i++) {
        int a = i * 10, b = i, res = -1;
        struct iovec params[2] = { { &a, sizeof(a) }, { &b, sizeof(b) } };
        struct iovec result    = { &res, sizeof(res) };

        async_req_id_t id = async_func(worker, fn_add, params, 2, &result);
        if (id == ASYNC_REQ_INVALID) { all_pass = 0; break; }

        async_worker_wait(worker, id);

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
 * Creates one async_worker_t (spawns async_worker_thread + async_func_thread),
 * runs all five tests, destroys the handle, and prints a summary.
 *
 * @return EXIT_SUCCESS when all assertions pass, EXIT_FAILURE otherwise.
 */
int main(void)
{
    printf("=== async_func test suite (request queue) ===\n");

    async_worker_t *worker = async_worker_init();
    if (!worker) {
        perror("async_worker_init");
        return EXIT_FAILURE;
    }

    test_single(worker);
    test_queue_fifo(worker);
    test_status(worker);
    test_queue_full(worker);
    test_sequential(worker);

    async_worker_destroy(worker);

    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
