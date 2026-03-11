/**
 * @file async_func.h
 * @brief Non-blocking async function dispatcher with a request queue and
 *        per-request status/result retrieval by request ID.
 *
 * Architecture
 * ------------
 * The subsystem consists of two persistent threads and a shared-memory block
 * (mmap MAP_ANONYMOUS | MAP_SHARED) holding a fixed-depth request queue:
 *
 *  ┌──────────┐  async_func()  ┌──────────────────┐  dispatch  ┌──────────────────┐
 *  │  Caller  │ ─────────────► │ async_worker      │ ─────────► │ async_func       │
 *  │          │  req_id        │ thread            │            │ thread           │
 *  │          │ ◄───────────── │ (dispatcher)      │            │ (executor)       │
 *  └──────────┘                └──────────────────┘            └──────────────────┘
 *       │
 *       │  async_worker_status(id)  ──► BUSY / FREE(READY)
 *       │
 *       │  async_get_result(id, buf) ──► copies result, releases slot
 *
 * No blocking wait exists on the caller side.  The caller submits a request,
 * receives an opaque request ID, periodically polls the status, and retrieves
 * the result with a single non-blocking call once the status is FREE (ready).
 *
 * Request queue
 * -------------
 * A fixed array of @c ASYNC_QUEUE_DEPTH request slots.  Each slot stores the
 * function pointer, input parameters, and the execution result inline
 * (no external allocation needed).  Slots progress through:
 *
 *   SLOT_FREE → SLOT_QUEUED → SLOT_RUNNING → SLOT_DONE
 *                                                 │
 *                              async_get_result() releases back to SLOT_FREE
 *
 * Typical usage
 * -------------
 * @code
 *   async_worker_t *w = async_worker_init();
 *
 *   // Prepare input parameters
 *   int a = 7, b = 3;
 *   struct iovec params[2] = {
 *       { .iov_base = &a, .iov_len = sizeof(a) },
 *       { .iov_base = &b, .iov_len = sizeof(b) },
 *   };
 *
 *   // Submit — returns immediately with a request ID
 *   async_req_id_t id = async_func(w, my_add_fn, params, 2);
 *   if (id == ASYNC_REQ_INVALID) { ... } // queue full
 *
 *   // Poll without blocking
 *   while (async_worker_status(w, id) == ASYNC_STATUS_BUSY)
 *       usleep(1000);
 *
 *   // Retrieve result and release the slot
 *   int result = 0;
 *   struct iovec out = { .iov_base = &result, .iov_len = sizeof(result) };
 *   async_get_result(w, id, &out);
 *   printf("result = %d\n", result);
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
 * Larger objects should be passed by pointer.
 */
#define ASYNC_MAX_DATA_SIZE 4096

/** Number of request slots in the queue (maximum outstanding requests). */
#define ASYNC_QUEUE_DEPTH   16

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/**
 * @brief Per-request execution status.
 */
typedef enum {
    ASYNC_STATUS_FREE = 0, /**< Request is complete; result ready for retrieval
                                via async_get_result().  Also returned for
                                unknown IDs (already released or never valid). */
    ASYNC_STATUS_BUSY = 1  /**< Request is queued or currently executing.      */
} async_status_t;

/**
 * @brief Unique identifier for a submitted async request.
 *
 * Assigned by async_func().  Remains valid until async_get_result() releases
 * the slot.  @c ASYNC_REQ_INVALID (0) is returned on queue-full failure.
 */
typedef uint32_t async_req_id_t;

/** Sentinel returned by async_func() when the queue is full. */
#define ASYNC_REQ_INVALID  ((async_req_id_t)0)

/**
 * @brief Prototype for functions that can be dispatched via async_func().
 *
 * @param[in]  params   Array of @p nparams iovec structures.  iov_base points
 *                      to parameter data in shared memory; iov_len is its size.
 * @param[in]  nparams  Number of valid entries in @p params.
 * @param[out] result   Pre-loaded with a shared-memory buffer of iov_len bytes.
 *                      The function writes its return value there and sets
 *                      iov_len to the number of bytes actually written.
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
 * Allocates the shared-memory block, initialises synchronisation objects with
 * PTHREAD_PROCESS_SHARED, and spawns async_worker_thread (dispatcher) and
 * async_func_thread (executor).
 *
 * @return Opaque handle on success, @c NULL on failure (errno set).
 */
async_worker_t *async_worker_init(void);

/**
 * @brief Destroy the async subsystem and free all resources.
 *
 * Sets the shutdown flag, wakes both threads, joins them, destroys all POSIX
 * synchronisation objects, and unmaps the shared-memory region.  Any queued
 * or in-progress requests are abandoned.
 *
 * @param[in] worker  Handle from async_worker_init().  @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker);

/**
 * @brief Submit a function for asynchronous execution (non-blocking).
 *
 * Finds a free slot in the request queue, deep-copies the function pointer
 * and all parameter data into shared memory, and appends the slot to the FIFO.
 * Returns immediately with a unique request ID; the caller is never blocked.
 *
 * The execution result is stored inside the slot.  Retrieve it with
 * async_get_result() once async_worker_status() returns ASYNC_STATUS_FREE.
 *
 * @param[in]  worker   Handle from async_worker_init().
 * @param[in]  func     Function to execute; must match async_fn_t.
 * @param[in]  params   Array of @p nparams iovec descriptors.  Data is
 *                      deep-copied before this call returns.
 * @param[in]  nparams  Number of entries in @p params (0..ASYNC_MAX_PARAMS).
 *
 * @return A non-zero @c async_req_id_t identifying this request, or
 *         @c ASYNC_REQ_INVALID if all @c ASYNC_QUEUE_DEPTH slots are occupied.
 */
async_req_id_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams);

/**
 * @brief Query the execution status of a specific request (non-blocking).
 *
 * @param[in] worker  Handle from async_worker_init().
 * @param[in] id      Request ID returned by async_func().
 *
 * @retval ASYNC_STATUS_BUSY  The request is queued or currently executing.
 * @retval ASYNC_STATUS_FREE  The request is complete and its result is
 *                            available via async_get_result(), or @p id was
 *                            not found (already released or never valid).
 */
async_status_t async_worker_status(async_worker_t *worker, async_req_id_t id);

/**
 * @brief Retrieve the result of a completed request and release its slot.
 *
 * Non-blocking.  Succeeds only when the request identified by @p id has
 * reached the DONE state (i.e. async_worker_status() returned FREE).
 *
 * On success the slot is released back to the free pool; the ID becomes
 * invalid and subsequent calls with the same ID return -1.
 *
 * @param[in]  worker  Handle from async_worker_init().
 * @param[in]  id      Request ID returned by async_func().
 * @param[out] result  Caller-allocated iovec.  iov_base must point to a buffer
 *                     of at least iov_len bytes.  On return iov_base contains
 *                     the function's output and iov_len is set to the number of
 *                     bytes written (capped at the original iov_len).
 *                     May be @c NULL if the return value is not needed.
 *
 * @retval  0   Result copied successfully; slot released.
 * @retval -1   Request not found, or still executing (not yet DONE).
 */
int async_get_result(async_worker_t *worker,
                     async_req_id_t  id,
                     struct iovec   *result);

#endif /* ASYNC_FUNC_H */
