/**
 * @file async_func.h
 * @brief Asynchronous function dispatcher with a request queue and per-request
 *        status tracking.
 *
 * Architecture
 * ------------
 * The subsystem consists of two persistent threads and a shared-memory block
 * (mmap MAP_ANONYMOUS | MAP_SHARED) that holds a fixed-depth request queue:
 *
 *  ┌──────────┐  async_func() ┌──────────────────┐  dispatch  ┌──────────────────┐
 *  │  Caller  │ ────────────► │ async_worker      │ ─────────► │ async_func       │
 *  │          │  req_id       │ thread            │            │ thread           │
 *  │          │ ◄──────────── │ (dispatcher)      │            │ (executor)       │
 *  └──────────┘               └──────────────────┘            └──────────────────┘
 *       │  async_worker_wait(id)                                       │
 *       └──────────────────────── waits on cond_done ◄────────────────┘
 *                                 (result in caller's buffer)
 *
 * Request queue
 * -------------
 * The queue is a fixed array of @c ASYNC_QUEUE_DEPTH request slots.  Each slot
 * carries one execution request and progresses through these states:
 *
 *   SLOT_FREE → SLOT_QUEUED → SLOT_RUNNING → SLOT_DONE → SLOT_FREE
 *
 * Each submitted request is assigned a unique @c async_req_id_t.  Callers use
 * this ID to query per-request status or to wait for a specific result.
 *
 * Concurrency model
 * -----------------
 *  - @c async_func() finds a free slot, fills it, appends its index to the
 *    FIFO queue, signals async_worker_thread, and returns the request ID
 *    immediately without blocking.
 *  - @c async_worker_thread dequeues the next slot when async_func_thread is
 *    idle, sets it to SLOT_RUNNING, and dispatches it.
 *  - @c async_func_thread executes the function, copies the result into the
 *    caller-supplied buffer, sets the slot to SLOT_DONE, and broadcasts
 *    @c cond_done to wake any @c async_worker_wait() callers.
 *  - @c async_worker_wait(id) blocks on @c cond_done until the target slot
 *    reaches SLOT_DONE, then auto-releases the slot back to SLOT_FREE.
 *  - @c async_worker_status(id) returns ASYNC_STATUS_BUSY (QUEUED or RUNNING)
 *    or ASYNC_STATUS_FREE (DONE); returns FREE for unknown IDs (already released).
 *
 * Typical usage
 * -------------
 * @code
 *   async_worker_t *w = async_worker_init();
 *
 *   // Prepare parameters
 *   int a = 7, b = 3, res = 0;
 *   struct iovec params[2] = {
 *       { .iov_base = &a, .iov_len = sizeof(a) },
 *       { .iov_base = &b, .iov_len = sizeof(b) },
 *   };
 *   struct iovec result = { .iov_base = &res, .iov_len = sizeof(res) };
 *
 *   // Submit — returns immediately
 *   async_req_id_t id = async_func(w, my_add_fn, params, 2, &result);
 *   if (id == ASYNC_REQ_INVALID) { ... } // queue full
 *
 *   // Poll (non-blocking)
 *   async_status_t st = async_worker_status(w, id);  // BUSY or FREE
 *
 *   // Block until done, then release the slot
 *   async_worker_wait(w, id);
 *   printf("result = %d\n", res);   // result buffer is now populated
 *
 *   async_worker_destroy(w);
 * @endcode
 */

#ifndef ASYNC_FUNC_H
#define ASYNC_FUNC_H

#include <sys/uio.h>   /* struct iovec */
#include <stdbool.h>
#include <stdint.h>

/** Maximum number of input parameters per request. */
#define ASYNC_MAX_PARAMS    16

/**
 * Maximum byte size for a single parameter slot or the result buffer.
 * Larger objects should be passed by pointer (the pointer itself fits here).
 */
#define ASYNC_MAX_DATA_SIZE 4096

/** Number of request slots in the queue; sets the maximum concurrency depth. */
#define ASYNC_QUEUE_DEPTH   16

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/**
 * @brief Per-request execution status as seen by the caller.
 */
typedef enum {
    ASYNC_STATUS_FREE = 0, /**< Request is complete (or ID not found).       */
    ASYNC_STATUS_BUSY = 1  /**< Request is queued or currently executing.     */
} async_status_t;

/**
 * @brief Unique identifier for a submitted async request.
 *
 * The value @c ASYNC_REQ_INVALID (0) is returned by @c async_func() when the
 * request queue is full and the submission was rejected.
 */
typedef uint32_t async_req_id_t;

/** Sentinel value returned when a submission fails (queue full). */
#define ASYNC_REQ_INVALID  ((async_req_id_t)0)

/**
 * @brief Prototype for functions that can be dispatched via async_func().
 *
 * @param[in]  params   Array of @p nparams iovec structures.  iov_base points
 *                      to parameter data (in shared memory); iov_len is its
 *                      byte size.
 * @param[in]  nparams  Number of valid entries in @p params.
 * @param[out] result   Pre-loaded with a shared-memory buffer (iov_base) of
 *                      iov_len bytes capacity.  The function writes its return
 *                      value there and updates iov_len to bytes actually written.
 */
typedef void (*async_fn_t)(struct iovec *params, int nparams,
                           struct iovec *result);

/** Opaque handle representing the two-thread async subsystem + request queue. */
typedef struct async_worker async_worker_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Allocates the shared-memory block (holding the request queue and all POSIX
 * synchronisation objects), initialises them with PTHREAD_PROCESS_SHARED, and
 * spawns async_worker_thread (dispatcher) and async_func_thread (executor).
 *
 * @return Opaque handle on success, @c NULL on failure (errno set).
 */
async_worker_t *async_worker_init(void);

/**
 * @brief Destroy the async subsystem and release all resources.
 *
 * Sets the shutdown flag, wakes both threads, joins them, destroys POSIX
 * objects, and unmaps the shared-memory region.  Any queued or in-progress
 * requests are abandoned; their result buffers may not be populated.
 *
 * @param[in] worker  Handle from async_worker_init().  @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker);

/**
 * @brief Submit a function for asynchronous execution (non-blocking).
 *
 * Finds a free slot in the request queue, deep-copies the function pointer and
 * all parameter data into shared memory, and appends the slot to the FIFO.
 * Returns immediately with the assigned request ID.
 *
 * The function executes asynchronously in async_func_thread.  The result is
 * written to @p result->iov_base once async_worker_wait() returns.
 *
 * @param[in]  worker   Handle from async_worker_init().
 * @param[in]  func     Function to execute; must match async_fn_t.
 * @param[in]  params   Array of @p nparams iovec descriptors.  Data is
 *                      deep-copied before this call returns.
 * @param[in]  nparams  Number of entries in @p params (0..ASYNC_MAX_PARAMS).
 * @param[out] result   Caller-allocated iovec.  iov_base must point to a buffer
 *                      of at least iov_len bytes.  Populated when the associated
 *                      async_worker_wait() call returns.  May be @c NULL.
 *
 * @return A non-zero @c async_req_id_t identifying this request, or
 *         @c ASYNC_REQ_INVALID if the queue is full.
 */
async_req_id_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result);

/**
 * @brief Query the execution status of a specific request (non-blocking).
 *
 * @param[in] worker  Handle from async_worker_init().
 * @param[in] id      Request ID returned by async_func().
 *
 * @retval ASYNC_STATUS_BUSY  The request is queued or currently executing.
 * @retval ASYNC_STATUS_FREE  The request is complete, or @p id was not found
 *                            (e.g. it was already released by async_worker_wait()).
 */
async_status_t async_worker_status(async_worker_t *worker, async_req_id_t id);

/**
 * @brief Block until a specific request completes, then release its slot.
 *
 * Waits on the internal @c cond_done condition until the slot associated with
 * @p id reaches the DONE state.  On return:
 *  - The result buffer passed to the original async_func() call is fully
 *    populated.
 *  - The request slot is released back to the free pool (the ID becomes
 *    invalid; async_worker_status() will return FREE for it).
 *
 * If @p id is not found (already released or never valid) this returns
 * immediately.
 *
 * @param[in] worker  Handle from async_worker_init().
 * @param[in] id      Request ID returned by async_func().
 */
void async_worker_wait(async_worker_t *worker, async_req_id_t id);

#endif /* ASYNC_FUNC_H */
