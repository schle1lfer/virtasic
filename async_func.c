/**
 * @file async_func.c
 * @brief Implementation of the async function dispatcher.
 *
 * The shared-memory block (async_shm_t) is created with
 *   mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0)
 * so it is accessible to both the spawning thread and any forked children.
 * All POSIX mutex / condition-variable objects are initialised with the
 * PTHREAD_PROCESS_SHARED attribute for the same reason.
 *
 * Concurrency model
 * -----------------
 *  - Only ONE outstanding request is supported at a time (single worker).
 *  - async_func() is non-blocking: it either posts the request and returns
 *    ASYNC_STATUS_FREE, or finds the worker busy and returns ASYNC_STATUS_BUSY.
 *  - The worker thread runs in a loop: sleep on cond_work → execute → signal
 *    cond_done → back to sleep.
 *  - The caller retrieves the result by calling async_worker_wait(), which
 *    blocks on cond_done and then copies shm->result_data into the buffer
 *    that was passed via the result iovec to async_func().
 */

#include "async_func.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

/* Byte budget for one parameter slot (total param storage / max params). */
#define PARAM_SLOT_SIZE  (ASYNC_MAX_DATA_SIZE / ASYNC_MAX_PARAMS)

/* -------------------------------------------------------------------------
 * Internal shared-memory layout
 * ---------------------------------------------------------------------- */

/**
 * @brief Shared-memory block exchanged between the caller and the worker.
 *
 * The entire struct lives in a region mapped with MAP_ANONYMOUS | MAP_SHARED,
 * making it accessible to threads AND forked processes.
 */
typedef struct {
    /* --- request (written by async_func, read by worker) --------------- */
    async_fn_t   func_ptr;                                /**< Function to run          */
    int          num_params;                              /**< Entries used in params[] */
    struct iovec params[ASYNC_MAX_PARAMS];                /**< Parameter descriptors    */
    uint8_t      param_data[ASYNC_MAX_PARAMS]             /**< Inline parameter bytes   */
                           [PARAM_SLOT_SIZE];

    /* --- result (written by worker, harvested by async_worker_wait) ---- */
    struct iovec result;                                  /**< Result descriptor        */
    uint8_t      result_data[ASYNC_MAX_DATA_SIZE];        /**< Inline result bytes      */

    /* --- caller's output buffer (set once per async_func call) --------- */
    struct iovec *caller_result; /**< Where to copy result on wait(); may be NULL */

    /* --- state flags --------------------------------------------------- */
    async_status_t status;           /**< FREE or BUSY                          */
    bool           request_pending;  /**< TRUE between post and worker pick-up  */
    bool           result_ready;     /**< TRUE after worker stores result       */
    bool           shutdown;         /**< TRUE → worker exits its loop          */

    /* --- POSIX synchronisation (PTHREAD_PROCESS_SHARED) ---------------- */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_work; /**< Caller  → worker : new request available  */
    pthread_cond_t  cond_done; /**< Worker  → caller : result ready           */
} async_shm_t;

/**
 * @brief Opaque worker handle (definition hidden from callers via typedef).
 */
struct async_worker {
    async_shm_t *shm; /**< Pointer into the mmap'd shared-memory region */
    pthread_t    tid; /**< Worker thread ID                              */
};

/* -------------------------------------------------------------------------
 * Worker thread
 * ---------------------------------------------------------------------- */

/**
 * @brief Background worker thread entry point.
 *
 * Runs an infinite loop:
 *  1. Waits on @c cond_work until a request is posted or shutdown is set.
 *  2. Snapshots the request fields under the lock, then releases the lock.
 *  3. Executes the requested function outside the lock so the caller is not
 *     blocked during execution.
 *  4. Stores the result in @c shm->result_data, marks status FREE, and
 *     broadcasts @c cond_done.
 *
 * @param[in] arg  Pointer to the @c async_shm_t shared-memory block.
 * @return Always @c NULL.
 */
static void *async_worker_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        /* ---- wait for work ------------------------------------------- */
        pthread_mutex_lock(&shm->mutex);

        while (!shm->request_pending && !shm->shutdown)
            pthread_cond_wait(&shm->cond_work, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        /* Snapshot request while holding the lock */
        async_fn_t   func    = shm->func_ptr;
        int          np      = shm->num_params;
        struct iovec lparams[ASYNC_MAX_PARAMS];
        memcpy(lparams, shm->params, (size_t)np * sizeof(struct iovec));

        shm->request_pending = false;
        pthread_mutex_unlock(&shm->mutex);

        /* ---- execute outside the lock --------------------------------- */
        /*
         * Provide the worker-owned result buffer; the function writes there.
         * iov_len is set to the capacity on entry; the function shrinks it
         * to the actual number of bytes written.
         */
        struct iovec local_result = {
            .iov_base = shm->result_data,
            .iov_len  = ASYNC_MAX_DATA_SIZE
        };

        func(lparams, np, &local_result);

        /* ---- publish result ------------------------------------------ */
        pthread_mutex_lock(&shm->mutex);

        shm->result.iov_base = shm->result_data;
        shm->result.iov_len  = local_result.iov_len;

        /* Copy result into the caller-supplied buffer if one was given */
        if (shm->caller_result != NULL) {
            size_t copy = shm->result.iov_len;
            if (copy > shm->caller_result->iov_len)
                copy = shm->caller_result->iov_len;
            memcpy(shm->caller_result->iov_base, shm->result_data, copy);
            shm->caller_result->iov_len = copy;
        }

        shm->result_ready = true;
        shm->status       = ASYNC_STATUS_FREE;

        pthread_cond_broadcast(&shm->cond_done);
        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Allocates a shared-memory region, initialises synchronisation objects with
 * the PTHREAD_PROCESS_SHARED attribute, and spawns the worker thread.
 *
 * @return Pointer to an opaque @c async_worker_t handle on success.
 *         @c NULL on failure (errno set by the failing call).
 */
async_worker_t *async_worker_init(void)
{
    /* Allocate the outer handle */
    async_worker_t *worker = malloc(sizeof(*worker));
    if (!worker)
        return NULL;

    /* Map shared memory: anonymous + shared so both threads and fork()d
     * children can reach it without a named shm object.                  */
    async_shm_t *shm = mmap(NULL, sizeof(async_shm_t),
                             PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_SHARED,
                             -1, 0);
    if (shm == MAP_FAILED) {
        free(worker);
        return NULL;
    }

    memset(shm, 0, sizeof(*shm));
    shm->status = ASYNC_STATUS_FREE;

    /* Initialise mutex and condition variables as process-shared */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(&shm->mutex, &mattr) != 0) {
        pthread_mutexattr_destroy(&mattr);
        munmap(shm, sizeof(*shm));
        free(worker);
        return NULL;
    }
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    if (pthread_cond_init(&shm->cond_work, &cattr) != 0 ||
        pthread_cond_init(&shm->cond_done, &cattr) != 0) {
        pthread_condattr_destroy(&cattr);
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, sizeof(*shm));
        free(worker);
        return NULL;
    }
    pthread_condattr_destroy(&cattr);

    worker->shm = shm;

    /* Spawn the worker thread */
    if (pthread_create(&worker->tid, NULL, async_worker_thread, shm) != 0) {
        pthread_cond_destroy(&shm->cond_done);
        pthread_cond_destroy(&shm->cond_work);
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, sizeof(*shm));
        free(worker);
        return NULL;
    }

    return worker;
}

/**
 * @brief Destroy the async subsystem and free all resources.
 *
 * Sets the shutdown flag, wakes the worker thread, joins it, then destroys
 * all synchronisation objects and unmaps shared memory.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *                    Passing @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker)
{
    if (!worker)
        return;

    async_shm_t *shm = worker->shm;

    /* Signal the worker to exit */
    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = true;
    pthread_cond_signal(&shm->cond_work);
    pthread_mutex_unlock(&shm->mutex);

    pthread_join(worker->tid, NULL);

    pthread_cond_destroy(&shm->cond_done);
    pthread_cond_destroy(&shm->cond_work);
    pthread_mutex_destroy(&shm->mutex);

    munmap(shm, sizeof(*shm));
    free(worker);
}

/**
 * @brief Submit a function for asynchronous execution.
 *
 * Copies the function pointer, parameter count, and all parameter data into
 * the shared-memory block, then signals the worker thread.  Returns
 * immediately without waiting for execution to complete.
 *
 * @param[in]  worker   Handle returned by async_worker_init().
 * @param[in]  func     Function to execute; must match async_fn_t.
 * @param[in]  params   Array of @p nparams iovec descriptors.  Data is deep-
 *                      copied before the call returns; the caller may reuse
 *                      its buffers immediately.
 * @param[in]  nparams  Number of entries in @p params.
 * @param[out] result   Caller-allocated iovec for the return value.  Must
 *                      have iov_base pointing to a buffer of at least
 *                      iov_len bytes.  Populated after async_worker_wait()
 *                      returns.  May be @c NULL.
 *
 * @retval ASYNC_STATUS_FREE  Request accepted; worker is now executing @p func.
 * @retval ASYNC_STATUS_BUSY  Worker already busy; request rejected.
 */
async_status_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);

    /* Reject if the worker has not finished its previous job */
    if (shm->status == ASYNC_STATUS_BUSY || shm->request_pending) {
        pthread_mutex_unlock(&shm->mutex);
        return ASYNC_STATUS_BUSY;
    }

    /* Write function pointer and parameter count into shared memory */
    shm->func_ptr   = func;
    shm->num_params = nparams;

    /* Deep-copy each parameter into the embedded storage slots */
    for (int i = 0; i < nparams; i++) {
        size_t sz = params[i].iov_len;
        if (sz > PARAM_SLOT_SIZE)
            sz = PARAM_SLOT_SIZE;
        memcpy(shm->param_data[i], params[i].iov_base, sz);
        shm->params[i].iov_base = shm->param_data[i];
        shm->params[i].iov_len  = sz;
    }

    /* Record where the caller wants the result copied once execution ends */
    shm->caller_result = result;
    shm->result_ready  = false;

    /* Mark busy and wake the worker */
    shm->status          = ASYNC_STATUS_BUSY;
    shm->request_pending = true;
    pthread_cond_signal(&shm->cond_work);

    pthread_mutex_unlock(&shm->mutex);

    /*
     * Return FREE to indicate "the submission slot was free and the request
     * was accepted".  The worker is now running; the caller can check
     * async_worker_status() to poll or call async_worker_wait() to block.
     */
    return ASYNC_STATUS_FREE;
}

/**
 * @brief Query the current status of the worker thread (non-blocking).
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *
 * @retval ASYNC_STATUS_FREE  Worker is idle.
 * @retval ASYNC_STATUS_BUSY  Worker is executing a function.
 */
async_status_t async_worker_status(async_worker_t *worker)
{
    async_shm_t   *shm = worker->shm;
    async_status_t st;

    pthread_mutex_lock(&shm->mutex);
    st = shm->status;
    pthread_mutex_unlock(&shm->mutex);

    return st;
}

/**
 * @brief Block until the worker finishes the current job.
 *
 * Waits on @c cond_done.  On return the result buffer that was passed to
 * the preceding async_func() call is fully populated.  If the worker is
 * already idle this function returns immediately.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 */
void async_worker_wait(async_worker_t *worker)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);
    while (shm->status == ASYNC_STATUS_BUSY)
        pthread_cond_wait(&shm->cond_done, &shm->mutex);
    pthread_mutex_unlock(&shm->mutex);
}
