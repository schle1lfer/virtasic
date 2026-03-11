/**
 * @file async_func.h
 * @brief Asynchronous function dispatcher using shared memory and a worker thread.
 *
 * Provides a single background worker thread that executes arbitrary functions
 * asynchronously.  The function pointer, parameter count, and parameters are
 * communicated through a shared-memory block (allocated with
 * mmap(MAP_ANONYMOUS | MAP_SHARED)) so the same mechanism works across both
 * threads and forked child processes.
 *
 * Typical usage:
 * @code
 *   async_worker_t *w = async_worker_init();
 *
 *   // Prepare parameters as an iovec array
 *   int a = 3, b = 5;
 *   struct iovec params[2] = {
 *       { .iov_base = &a, .iov_len = sizeof(a) },
 *       { .iov_base = &b, .iov_len = sizeof(b) },
 *   };
 *
 *   // Prepare a result buffer
 *   int result_val = 0;
 *   struct iovec result = { .iov_base = &result_val, .iov_len = sizeof(result_val) };
 *
 *   async_status_t st = async_func(w, my_add_fn, params, 2, &result);
 *   if (st == ASYNC_STATUS_FREE) {
 *       async_worker_wait(w);   // block until done
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
 * @brief Execution status of the async worker thread.
 */
typedef enum {
    ASYNC_STATUS_FREE = 0, /**< Worker is idle; the last async_func() call was
                                accepted and this status was returned to signal
                                that the submission slot was available.       */
    ASYNC_STATUS_BUSY = 1  /**< Worker is currently executing a function; the
                                async_func() call was rejected.               */
} async_status_t;

/**
 * @brief Prototype for functions that can be dispatched via async_func().
 *
 * @param[in]  params   Array of @p nparams iovec structures.  Each element's
 *                      iov_base points to the parameter data and iov_len gives
 *                      the byte size.  The memory is owned by the shared-memory
 *                      block for the duration of the call.
 * @param[in]  nparams  Number of valid entries in @p params.
 * @param[out] result   Output descriptor.  On entry iov_base already points to
 *                      a pre-allocated buffer of iov_len bytes inside the
 *                      shared-memory block.  The function must write its return
 *                      value there and update iov_len to the number of bytes
 *                      actually written.
 */
typedef void (*async_fn_t)(struct iovec *params, int nparams,
                           struct iovec *result);

/** Opaque handle representing one async worker + shared-memory pair. */
typedef struct async_worker async_worker_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Allocates a shared-memory region (via mmap) that holds the communication
 * structure, initialises POSIX synchronisation objects with the
 * PTHREAD_PROCESS_SHARED attribute, and spawns the background worker thread.
 *
 * @return Pointer to an opaque worker handle on success, or @c NULL on failure
 *         (errno is set by the failing system call).
 */
async_worker_t *async_worker_init(void);

/**
 * @brief Destroy the async subsystem and release all resources.
 *
 * Signals the worker thread to exit, waits for it to finish, destroys POSIX
 * synchronisation objects, and unmaps the shared-memory region.
 *
 * @param[in] worker  Handle returned by async_worker_init().  Passing @c NULL
 *                    is a no-op.
 */
void async_worker_destroy(async_worker_t *worker);

/**
 * @brief Submit a function for asynchronous execution (non-blocking).
 *
 * Writes the function pointer, the number of parameters, and a deep copy of
 * the parameter data into the shared-memory block, then wakes the worker
 * thread.  Returns immediately without waiting for the function to complete.
 *
 * @param[in]  worker   Handle returned by async_worker_init().
 * @param[in]  func     Function to execute asynchronously; must match the
 *                      async_fn_t prototype.
 * @param[in]  params   Array of @p nparams iovec descriptors.  Each
 *                      iov_base/iov_len pair describes one input argument.
 *                      The data is copied into shared memory before this call
 *                      returns, so the caller may reuse its buffers afterwards.
 * @param[in]  nparams  Number of entries in @p params (0 .. ASYNC_MAX_PARAMS).
 * @param[out] result   iovec that will receive the function's return value once
 *                      async_worker_wait() returns.  The caller must
 *                      pre-allocate result->iov_base and set result->iov_len to
 *                      the size of that buffer before calling async_func().
 *                      May be @c NULL if the return value is not needed.
 *
 * @retval ASYNC_STATUS_FREE  The worker was idle; the request has been
 *                            accepted and the worker is now executing @p func.
 * @retval ASYNC_STATUS_BUSY  The worker is already executing a function; the
 *                            request was rejected and @p func was not called.
 */
async_status_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result);

/**
 * @brief Query the current status of the worker thread without blocking.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *
 * @retval ASYNC_STATUS_FREE  Worker is idle.
 * @retval ASYNC_STATUS_BUSY  Worker is executing a function.
 */
async_status_t async_worker_status(async_worker_t *worker);

/**
 * @brief Block the calling thread until the worker finishes the current job.
 *
 * After this call returns the result buffer passed to the preceding
 * async_func() call contains the function's return value.  If the worker is
 * already idle this function returns immediately.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 */
void async_worker_wait(async_worker_t *worker);

#endif /* ASYNC_FUNC_H */
