/**
 * @file async_func.h
 * @brief Asynchronous function dispatcher using shared memory and two worker threads.
 *
 * Architecture
 * ------------
 * Three participants communicate through a single shared-memory block
 * (mmap(MAP_ANONYMOUS | MAP_SHARED)):
 *
 *  ┌────────────┐  async_func()   ┌──────────────────┐  dispatch   ┌──────────────────┐
 *  │   Caller   │ ─────────────►  │ async_worker      │ ──────────► │ async_func       │
 *  │  (any      │ ◄─────────────  │ thread            │             │ thread           │
 *  │   thread)  │  immediate      │ (dispatcher)      │             │ (executor)       │
 *  └────────────┘  BUSY/FREE ack  └──────────────────┘             └──────────────────┘
 *        │                                                                   │
 *        └───────────────────── async_worker_wait() ◄──────────────────────┘
 *                                  (result ready)
 *
 * Step-by-step flow:
 *  1. Caller writes func_ptr, params into shared memory and signals
 *     async_worker_thread via cond_submit.
 *  2. async_worker_thread wakes, reads the current status of async_func_thread:
 *       - BUSY  → posts ASYNC_STATUS_BUSY on cond_ack, goes back to sleep.
 *       - FREE  → forwards the request to async_func_thread via cond_dispatch,
 *                 then posts ASYNC_STATUS_FREE on cond_ack.
 *     This ack is posted before async_func_thread has executed anything.
 *  3. Caller receives the ack on cond_ack and async_func() returns immediately.
 *  4. async_func_thread executes the function, writes the result into shared
 *     memory, marks itself FREE, and broadcasts cond_done.
 *  5. Caller blocks on async_worker_wait() → cond_done until step 4 completes.
 *
 * Key property: async_worker_thread reports status to the caller before
 * async_func_thread begins execution, so async_func() always returns
 * immediately regardless of how long the dispatched function takes.
 *
 * Typical usage:
 * @code
 *   async_worker_t *w = async_worker_init();
 *
 *   int a = 7, b = 3;
 *   struct iovec params[2] = {
 *       { .iov_base = &a, .iov_len = sizeof(a) },
 *       { .iov_base = &b, .iov_len = sizeof(b) },
 *   };
 *
 *   int result_val = 0;
 *   struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };
 *
 *   async_status_t st = async_func(w, my_add_fn, params, 2, &result);
 *   // st is already ASYNC_STATUS_FREE or ASYNC_STATUS_BUSY here;
 *   // my_add_fn may still be running in async_func_thread.
 *   if (st == ASYNC_STATUS_FREE) {
 *       async_worker_wait(w);
 *       printf("result = %d\n", result_val);
 *   }
 *
 *   async_worker_destroy(w);
 * @endcode
 */

#ifndef ASYNC_FUNC_H
#define ASYNC_FUNC_H

#include <sys/uio.h>   /* struct iovec */
#include <stdbool.h>

/** Maximum number of function parameters supported per call. */
#define ASYNC_MAX_PARAMS    16

/**
 * Maximum byte size for a single parameter slot or the result buffer.
 * Larger objects must be passed by pointer (the pointer itself fits here).
 */
#define ASYNC_MAX_DATA_SIZE 4096

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/**
 * @brief Execution status reported by async_worker_thread to the caller.
 */
typedef enum {
    ASYNC_STATUS_FREE = 0, /**< async_func_thread was idle; the request was
                                forwarded and this value is returned to signal
                                that execution has been scheduled.            */
    ASYNC_STATUS_BUSY = 1  /**< async_func_thread was already executing a
                                function; the request was rejected.           */
} async_status_t;

/**
 * @brief Prototype for functions that can be dispatched via async_func().
 *
 * @param[in]  params   Array of @p nparams iovec structures.  Each element's
 *                      iov_base points to the parameter data and iov_len gives
 *                      the byte size.  The buffers reside in the shared-memory
 *                      block for the duration of the call.
 * @param[in]  nparams  Number of valid entries in @p params.
 * @param[out] result   Output descriptor pre-loaded with a shared-memory
 *                      buffer (iov_base) of iov_len bytes capacity.  The
 *                      function must write its return value there and update
 *                      iov_len to the number of bytes actually written.
 */
typedef void (*async_fn_t)(struct iovec *params, int nparams,
                           struct iovec *result);

/** Opaque handle representing the two-thread async subsystem. */
typedef struct async_worker async_worker_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Allocates a shared-memory region (mmap MAP_ANONYMOUS|MAP_SHARED) holding
 * the communication block, initialises all POSIX synchronisation objects with
 * the PTHREAD_PROCESS_SHARED attribute, and spawns two persistent threads:
 * async_worker_thread (dispatcher) and async_func_thread (executor).
 *
 * @return Pointer to an opaque worker handle on success, or @c NULL on failure
 *         (errno is set by the failing system call).
 */
async_worker_t *async_worker_init(void);

/**
 * @brief Destroy the async subsystem and release all resources.
 *
 * Sets the shutdown flag, wakes both threads, joins them, destroys all POSIX
 * synchronisation objects, and unmaps the shared-memory region.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *                    Passing @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker);

/**
 * @brief Submit a function for asynchronous execution.
 *
 * Writes the function pointer, parameter count, and a deep copy of all
 * parameter data into the shared-memory block, then signals async_worker_thread.
 * Blocks only until async_worker_thread has inspected async_func_thread's
 * status and posted its acknowledgement — not until the function finishes.
 *
 * @param[in]  worker   Handle returned by async_worker_init().
 * @param[in]  func     Function to execute asynchronously; must match async_fn_t.
 * @param[in]  params   Array of @p nparams iovec descriptors.  Each
 *                      iov_base/iov_len pair describes one input argument.
 *                      Data is deep-copied before this call returns; the
 *                      caller may reuse its buffers immediately afterwards.
 * @param[in]  nparams  Number of entries in @p params (0 .. ASYNC_MAX_PARAMS).
 * @param[out] result   Caller-allocated iovec for the return value.  Must have
 *                      iov_base pointing to a buffer of at least iov_len bytes.
 *                      Populated once async_worker_wait() returns.
 *                      May be @c NULL if the return value is not needed.
 *
 * @retval ASYNC_STATUS_FREE  async_func_thread was idle; @p func has been
 *                            forwarded and will execute asynchronously.
 *                            This status is returned before execution begins.
 * @retval ASYNC_STATUS_BUSY  async_func_thread is already running a function;
 *                            the request was rejected and @p func was not called.
 */
async_status_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result);

/**
 * @brief Query the current execution status of async_func_thread (non-blocking).
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *
 * @retval ASYNC_STATUS_FREE  async_func_thread is idle.
 * @retval ASYNC_STATUS_BUSY  async_func_thread is executing a function.
 */
async_status_t async_worker_status(async_worker_t *worker);

/**
 * @brief Block the calling thread until async_func_thread finishes its job.
 *
 * Waits on the internal cond_done condition.  On return the result buffer
 * that was passed to the preceding async_func() call is fully populated.
 * Returns immediately if async_func_thread is already idle.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 */
void async_worker_wait(async_worker_t *worker);

#endif /* ASYNC_FUNC_H */
