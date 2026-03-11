/**
 * @file async_func.c
 * @brief Two-thread asynchronous function dispatcher implementation.
 *
 * Concurrency model
 * -----------------
 * Two persistent threads communicate via a shared-memory block (async_shm_t)
 * allocated with mmap(MAP_ANONYMOUS | MAP_SHARED).  All POSIX mutex and
 * condition-variable objects carry the PTHREAD_PROCESS_SHARED attribute.
 *
 *  async_worker_thread  (dispatcher)
 *    Loop: wait on cond_submit → inspect func_status → if FREE dispatch to
 *    async_func_thread + ack FREE, else ack BUSY → signal cond_ack → repeat.
 *    The ack is sent before async_func_thread executes anything.
 *
 *  async_func_thread  (executor)
 *    Loop: wait on cond_dispatch → snapshot params → unlock → run function →
 *    copy result → set func_status = FREE → broadcast cond_done → repeat.
 *
 * Condition-variable map
 * ----------------------
 *  cond_submit   caller        → async_worker_thread  (new request posted)
 *  cond_ack      async_worker  → caller               (BUSY/FREE status reply)
 *  cond_dispatch async_worker  → async_func_thread    (work forwarded)
 *  cond_done     async_func    → caller               (result ready)
 */

#include "async_func.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

/** Byte budget per parameter slot. */
#define PARAM_SLOT_SIZE  (ASYNC_MAX_DATA_SIZE / ASYNC_MAX_PARAMS)

/* -------------------------------------------------------------------------
 * Shared-memory layout
 * ---------------------------------------------------------------------- */

/**
 * @brief Shared-memory block visible to the caller, async_worker_thread, and
 *        async_func_thread.  Lives in a MAP_ANONYMOUS | MAP_SHARED mmap region.
 */
typedef struct {
    /* --- submission (caller → async_worker_thread) --------------------- */
    async_fn_t   func_ptr;                               /**< Function to run          */
    int          num_params;                             /**< Entries used in params[] */
    struct iovec params[ASYNC_MAX_PARAMS];               /**< Parameter descriptors    */
    uint8_t      param_data[ASYNC_MAX_PARAMS]            /**< Inline parameter storage */
                           [PARAM_SLOT_SIZE];
    /*
     * pending_result: written by every caller in async_func() before posting
     *   the submission.  May be overwritten by a subsequent rejected call.
     * caller_result:  committed by async_worker_thread ONLY when it accepts a
     *   submission (FREE path).  This is the pointer async_func_thread uses
     *   to copy the result into the caller's buffer on completion.
     * Keeping them separate prevents a rejected second submission from
     * corrupting the result destination of an in-progress first call.
     */
    struct iovec *pending_result;  /**< Candidate output buffer from caller  */
    struct iovec *caller_result;   /**< Accepted output buffer (set by worker)*/
    bool          submit_pending;  /**< Caller has posted a new request      */

    /* --- acknowledgement (async_worker_thread → caller) ---------------- */
    async_status_t ack_status;    /**< BUSY or FREE reported by async_worker */
    bool           ack_ready;     /**< async_worker has posted the ack       */

    /* --- dispatch (async_worker_thread → async_func_thread) ------------ */
    bool dispatch_pending;        /**< async_worker has forwarded work        */

    /* --- result (async_func_thread → caller) --------------------------- */
    struct iovec result;                           /**< Result descriptor     */
    uint8_t      result_data[ASYNC_MAX_DATA_SIZE]; /**< Inline result storage */
    bool         result_ready;                     /**< Execution complete     */

    /* --- async_func_thread status -------------------------------------- */
    async_status_t func_status;   /**< FREE or BUSY; owned by async_func_thread */

    /* --- lifecycle ------------------------------------------------------ */
    bool shutdown;                /**< Set to true to stop both threads      */

    /* --- POSIX synchronisation (PTHREAD_PROCESS_SHARED) ---------------- */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_submit;   /**< caller        → async_worker_thread */
    pthread_cond_t  cond_ack;      /**< async_worker  → caller              */
    pthread_cond_t  cond_dispatch; /**< async_worker  → async_func_thread   */
    pthread_cond_t  cond_done;     /**< async_func    → caller              */
} async_shm_t;

/**
 * @brief Opaque worker handle (definition hidden from callers via typedef).
 */
struct async_worker {
    async_shm_t *shm;         /**< Shared-memory region                   */
    pthread_t    worker_tid;  /**< async_worker_thread ID (dispatcher)    */
    pthread_t    func_tid;    /**< async_func_thread ID   (executor)      */
};

/* -------------------------------------------------------------------------
 * async_worker_thread — dispatcher
 * ---------------------------------------------------------------------- */

/**
 * @brief Dispatcher thread: routes submissions from the caller to
 *        async_func_thread and immediately acknowledges each submission.
 *
 * For each request received on @c cond_submit:
 *  - If @c func_status is BUSY: post @c ASYNC_STATUS_BUSY on @c cond_ack.
 *  - If @c func_status is FREE: mark @c func_status BUSY, forward the
 *    request to async_func_thread via @c cond_dispatch, then post
 *    @c ASYNC_STATUS_FREE on @c cond_ack.
 *
 * The ack is always posted before async_func_thread begins execution,
 * so the caller receives the status immediately regardless of execution time.
 *
 * @param[in] arg  Pointer to @c async_shm_t.
 * @return Always @c NULL.
 */
static void *async_worker_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        pthread_mutex_lock(&shm->mutex);

        while (!shm->submit_pending && !shm->shutdown)
            pthread_cond_wait(&shm->cond_submit, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        shm->submit_pending = false;

        if (shm->func_status == ASYNC_STATUS_BUSY) {
            /* async_func_thread is occupied — reject the request.
             * Do NOT touch caller_result: a previous accepted call may still
             * be executing and will need the original pointer on completion. */
            shm->ack_status = ASYNC_STATUS_BUSY;
        } else {
            /* async_func_thread is free — commit the result destination and
             * forward the work.  Only here do we update caller_result so that
             * a previously rejected call cannot corrupt this pointer.        */
            shm->caller_result   = shm->pending_result;
            shm->result_ready    = false;
            shm->func_status     = ASYNC_STATUS_BUSY;
            shm->dispatch_pending = true;
            pthread_cond_signal(&shm->cond_dispatch);

            shm->ack_status = ASYNC_STATUS_FREE;
        }

        /* Acknowledge the caller immediately (before func finishes) */
        shm->ack_ready = true;
        pthread_cond_signal(&shm->cond_ack);

        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * async_func_thread — executor
 * ---------------------------------------------------------------------- */

/**
 * @brief Executor thread: runs the function forwarded by async_worker_thread.
 *
 * For each dispatch received on @c cond_dispatch:
 *  1. Snapshots func_ptr and params under the lock.
 *  2. Releases the lock and invokes the function.
 *  3. Re-acquires the lock to write the result and reset @c func_status to
 *     FREE, then broadcasts @c cond_done so the caller can collect the result.
 *
 * The function is called outside the lock so the caller and async_worker_thread
 * remain unblocked during long-running operations.
 *
 * @param[in] arg  Pointer to @c async_shm_t.
 * @return Always @c NULL.
 */
static void *async_func_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        pthread_mutex_lock(&shm->mutex);

        while (!shm->dispatch_pending && !shm->shutdown)
            pthread_cond_wait(&shm->cond_dispatch, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        shm->dispatch_pending = false;

        /* Snapshot request fields while holding the lock */
        async_fn_t   func = shm->func_ptr;
        int          np   = shm->num_params;
        struct iovec lparams[ASYNC_MAX_PARAMS];
        memcpy(lparams, shm->params, (size_t)np * sizeof(struct iovec));

        pthread_mutex_unlock(&shm->mutex);

        /* Execute the user function outside the lock */
        struct iovec local_result = {
            .iov_base = shm->result_data,
            .iov_len  = ASYNC_MAX_DATA_SIZE
        };
        func(lparams, np, &local_result);

        /* Publish the result */
        pthread_mutex_lock(&shm->mutex);

        shm->result.iov_base = shm->result_data;
        shm->result.iov_len  = local_result.iov_len;

        if (shm->caller_result != NULL) {
            size_t copy = shm->result.iov_len;
            if (copy > shm->caller_result->iov_len)
                copy = shm->caller_result->iov_len;
            memcpy(shm->caller_result->iov_base, shm->result_data, copy);
            shm->caller_result->iov_len = copy;
        }

        shm->result_ready = true;
        shm->func_status  = ASYNC_STATUS_FREE;

        pthread_cond_broadcast(&shm->cond_done);
        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Internal helper: initialise one condition variable as process-shared
 * ---------------------------------------------------------------------- */
static int init_cond(pthread_cond_t *cv, const pthread_condattr_t *ca)
{
    return pthread_cond_init(cv, ca);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Allocates a shared-memory region, initialises four condition variables and
 * one mutex with PTHREAD_PROCESS_SHARED, and spawns async_worker_thread and
 * async_func_thread.
 *
 * @return Opaque handle on success, @c NULL on failure (errno set).
 */
async_worker_t *async_worker_init(void)
{
    async_worker_t *worker = malloc(sizeof(*worker));
    if (!worker)
        return NULL;

    async_shm_t *shm = mmap(NULL, sizeof(async_shm_t),
                             PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_SHARED,
                             -1, 0);
    if (shm == MAP_FAILED) {
        free(worker);
        return NULL;
    }

    memset(shm, 0, sizeof(*shm));
    shm->func_status = ASYNC_STATUS_FREE;

    /* Mutex */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    int rc = pthread_mutex_init(&shm->mutex, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc != 0) goto err_munmap;

    /* Four condition variables */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);

    if (init_cond(&shm->cond_submit,   &ca) != 0 ||
        init_cond(&shm->cond_ack,      &ca) != 0 ||
        init_cond(&shm->cond_dispatch, &ca) != 0 ||
        init_cond(&shm->cond_done,     &ca) != 0) {
        pthread_condattr_destroy(&ca);
        goto err_mutex;
    }
    pthread_condattr_destroy(&ca);

    worker->shm = shm;

    /* Spawn dispatcher thread */
    if (pthread_create(&worker->worker_tid, NULL, async_worker_thread, shm) != 0)
        goto err_conds;

    /* Spawn executor thread */
    if (pthread_create(&worker->func_tid, NULL, async_func_thread, shm) != 0) {
        pthread_mutex_lock(&shm->mutex);
        shm->shutdown = true;
        pthread_cond_signal(&shm->cond_submit);
        pthread_mutex_unlock(&shm->mutex);
        pthread_join(worker->worker_tid, NULL);
        goto err_conds;
    }

    return worker;

err_conds:
    pthread_cond_destroy(&shm->cond_done);
    pthread_cond_destroy(&shm->cond_dispatch);
    pthread_cond_destroy(&shm->cond_ack);
    pthread_cond_destroy(&shm->cond_submit);
err_mutex:
    pthread_mutex_destroy(&shm->mutex);
err_munmap:
    munmap(shm, sizeof(*shm));
    free(worker);
    return NULL;
}

/**
 * @brief Destroy the async subsystem and free all resources.
 *
 * Sets the shutdown flag, wakes both threads (on their respective wait
 * conditions), joins them, then destroys synchronisation objects and unmaps
 * the shared-memory region.
 *
 * @param[in] worker  Handle returned by async_worker_init().  @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker)
{
    if (!worker)
        return;

    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = true;
    pthread_cond_broadcast(&shm->cond_submit);   /* wake async_worker_thread */
    pthread_cond_broadcast(&shm->cond_dispatch); /* wake async_func_thread   */
    pthread_mutex_unlock(&shm->mutex);

    pthread_join(worker->worker_tid, NULL);
    pthread_join(worker->func_tid,   NULL);

    pthread_cond_destroy(&shm->cond_done);
    pthread_cond_destroy(&shm->cond_dispatch);
    pthread_cond_destroy(&shm->cond_ack);
    pthread_cond_destroy(&shm->cond_submit);
    pthread_mutex_destroy(&shm->mutex);

    munmap(shm, sizeof(*shm));
    free(worker);
}

/**
 * @brief Submit a function for asynchronous execution.
 *
 * Deep-copies func_ptr and all parameters into shared memory, signals
 * async_worker_thread, then blocks on @c cond_ack until async_worker_thread
 * posts its immediate status reply.  Returns before async_func_thread begins
 * executing the function.
 *
 * @param[in]  worker   Handle returned by async_worker_init().
 * @param[in]  func     Function to execute; must match async_fn_t.
 * @param[in]  params   Array of @p nparams iovec descriptors; data is deep-copied.
 * @param[in]  nparams  Number of entries in @p params (0..ASYNC_MAX_PARAMS).
 * @param[out] result   Caller-allocated iovec populated after async_worker_wait().
 *                      May be @c NULL.
 *
 * @retval ASYNC_STATUS_FREE  async_func_thread was idle; execution scheduled.
 * @retval ASYNC_STATUS_BUSY  async_func_thread was busy; request rejected.
 */
async_status_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);

    /* Write request into shared memory */
    shm->func_ptr   = func;
    shm->num_params = nparams;

    for (int i = 0; i < nparams; i++) {
        size_t sz = params[i].iov_len;
        if (sz > PARAM_SLOT_SIZE)
            sz = PARAM_SLOT_SIZE;
        memcpy(shm->param_data[i], params[i].iov_base, sz);
        shm->params[i].iov_base = shm->param_data[i];
        shm->params[i].iov_len  = sz;
    }

    shm->pending_result = result;   /* async_worker_thread commits to caller_result on FREE */
    shm->ack_ready      = false;

    /* Signal async_worker_thread */
    shm->submit_pending = true;
    pthread_cond_signal(&shm->cond_submit);

    /* Wait for the immediate status ack from async_worker_thread.
     * This returns as soon as async_worker_thread has checked func_status
     * and dispatched (or rejected) the request — not when func finishes. */
    while (!shm->ack_ready)
        pthread_cond_wait(&shm->cond_ack, &shm->mutex);

    async_status_t st = shm->ack_status;
    pthread_mutex_unlock(&shm->mutex);

    return st;
}

/**
 * @brief Query the current execution status of async_func_thread (non-blocking).
 *
 * @param[in] worker  Handle returned by async_worker_init().
 *
 * @retval ASYNC_STATUS_FREE  async_func_thread is idle.
 * @retval ASYNC_STATUS_BUSY  async_func_thread is executing a function.
 */
async_status_t async_worker_status(async_worker_t *worker)
{
    async_shm_t   *shm = worker->shm;
    async_status_t st;

    pthread_mutex_lock(&shm->mutex);
    st = shm->func_status;
    pthread_mutex_unlock(&shm->mutex);

    return st;
}

/**
 * @brief Block until async_func_thread finishes its current job.
 *
 * Waits on @c cond_done.  On return the result buffer passed to the preceding
 * async_func() call is fully populated.  Returns immediately if
 * async_func_thread is already idle.
 *
 * @param[in] worker  Handle returned by async_worker_init().
 */
void async_worker_wait(async_worker_t *worker)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);
    while (shm->func_status == ASYNC_STATUS_BUSY)
        pthread_cond_wait(&shm->cond_done, &shm->mutex);
    pthread_mutex_unlock(&shm->mutex);
}
