/**
 * @file async_func.c
 * @brief Two-thread async dispatcher implementation with a request queue.
 *
 * Shared-memory layout
 * --------------------
 * One @c async_shm_t is mmap'd with MAP_ANONYMOUS | MAP_SHARED.  It contains:
 *  - @c slots[]          — fixed pool of @c ASYNC_QUEUE_DEPTH request slots.
 *  - @c q_idx[]          — FIFO queue of slot indices awaiting dispatch.
 *  - Synchronisation     — one mutex + three condition variables, all
 *                          initialised with PTHREAD_PROCESS_SHARED.
 *
 * Slot lifecycle
 * --------------
 *   SLOT_FREE
 *     │  async_func() claims the slot, deep-copies request data, enqueues.
 *     ▼
 *   SLOT_QUEUED
 *     │  async_worker_thread dequeues, marks slot and hands off to func_thread.
 *     ▼
 *   SLOT_RUNNING
 *     │  async_func_thread executes, copies result to caller's buffer.
 *     ▼
 *   SLOT_DONE
 *     │  async_worker_wait() collects result (already in caller's buffer),
 *     │  releases slot back to pool.
 *     ▼
 *   SLOT_FREE
 *
 * Condition-variable map
 * ----------------------
 *  cond_worker  — wakes async_worker_thread when:
 *                   (a) a new slot is enqueued, or
 *                   (b) async_func_thread just became idle (more work may wait).
 *  cond_func    — wakes async_func_thread when async_worker_thread has loaded
 *                 dispatch_slot with a slot index to execute.
 *  cond_done    — broadcast by async_func_thread when a slot reaches SLOT_DONE;
 *                 wakes any async_worker_wait() callers polling for that ID.
 */

#include "async_func.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

/** Byte capacity of one parameter storage slot. */
#define PARAM_SLOT_SIZE  (ASYNC_MAX_DATA_SIZE / ASYNC_MAX_PARAMS)

/* -------------------------------------------------------------------------
 * Request slot
 * ---------------------------------------------------------------------- */

/**
 * @brief Internal lifecycle state of a request slot.
 */
typedef enum {
    SLOT_FREE    = 0, /**< Available for a new submission.               */
    SLOT_QUEUED  = 1, /**< Enqueued; waiting for async_func_thread.      */
    SLOT_RUNNING = 2, /**< Currently executing in async_func_thread.     */
    SLOT_DONE    = 3  /**< Execution complete; result in caller's buffer. */
} slot_state_t;

/**
 * @brief One entry in the request queue.
 *
 * All parameter and result data is stored inline so no external allocations
 * are needed after the slot is filled by async_func().
 */
typedef struct {
    async_req_id_t  id;                                  /**< Unique request ID            */
    slot_state_t    state;                               /**< Lifecycle state              */

    /* --- request fields (written by async_func, read by async_func_thread) */
    async_fn_t      func_ptr;                            /**< Function to execute          */
    int             num_params;                          /**< Valid entries in params[]    */
    struct iovec    params[ASYNC_MAX_PARAMS];            /**< Parameter descriptors        */
    uint8_t         param_data[ASYNC_MAX_PARAMS]         /**< Inline parameter storage     */
                              [PARAM_SLOT_SIZE];

    /* --- result fields (written by async_func_thread) */
    struct iovec   *caller_result; /**< Caller's output buffer; may be NULL  */
    uint8_t         result_data[ASYNC_MAX_DATA_SIZE];    /**< Inline result storage        */
} async_slot_t;

/* -------------------------------------------------------------------------
 * Shared-memory block
 * ---------------------------------------------------------------------- */

/**
 * @brief Entire shared state exchanged between the caller thread,
 *        async_worker_thread, and async_func_thread.
 */
typedef struct {
    /* --- request slot pool --------------------------------------------- */
    async_slot_t    slots[ASYNC_QUEUE_DEPTH]; /**< Fixed slot pool              */

    /* --- FIFO dispatch queue (holds slot indices) ----------------------- */
    int             q_idx[ASYNC_QUEUE_DEPTH]; /**< Circular queue of slot idxs  */
    int             q_head;                   /**< Dequeue position             */
    int             q_tail;                   /**< Enqueue position             */
    int             q_count;                  /**< Current queue depth          */

    /* --- request ID generator ------------------------------------------ */
    async_req_id_t  next_id; /**< Monotonically increasing; 0 is skipped      */

    /* --- async_worker_thread → async_func_thread handoff --------------- */
    int             dispatch_slot; /**< Slot index to execute; -1 = none      */

    /* --- async_func_thread state --------------------------------------- */
    bool            func_busy; /**< TRUE while async_func_thread is executing  */

    /* --- lifecycle ------------------------------------------------------ */
    bool            shutdown;  /**< Set to TRUE to stop both threads           */

    /* --- POSIX synchronisation (PTHREAD_PROCESS_SHARED) ---------------- */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_worker; /**< → async_worker_thread (queue or idle)   */
    pthread_cond_t  cond_func;   /**< → async_func_thread   (new dispatch)    */
    pthread_cond_t  cond_done;   /**< → callers             (slot completed)  */
} async_shm_t;

/**
 * @brief Opaque handle (visible to callers only via the typedef).
 */
struct async_worker {
    async_shm_t *shm;        /**< Shared-memory region                       */
    pthread_t    worker_tid; /**< async_worker_thread (dispatcher)            */
    pthread_t    func_tid;   /**< async_func_thread   (executor)              */
};

/* -------------------------------------------------------------------------
 * async_worker_thread — dispatcher
 * ---------------------------------------------------------------------- */

/**
 * @brief Dispatcher thread: dequeues requests and hands them to
 *        async_func_thread one at a time.
 *
 * Waits on @c cond_worker until:
 *  - The FIFO queue is non-empty, AND
 *  - async_func_thread is idle (@c func_busy == false) AND
 *  - No dispatch is already pending (@c dispatch_slot == -1).
 *
 * On wake it dequeues the head slot, marks it SLOT_RUNNING, stores its index
 * in @c dispatch_slot, and signals @c cond_func to wake async_func_thread.
 *
 * @param[in] arg  Pointer to @c async_shm_t.
 * @return Always @c NULL.
 */
static void *async_worker_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        pthread_mutex_lock(&shm->mutex);

        /* Wait until there is queued work and the executor is free */
        while (!shm->shutdown &&
               (shm->q_count == 0 || shm->func_busy || shm->dispatch_slot >= 0))
            pthread_cond_wait(&shm->cond_worker, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        /* Dequeue the head slot */
        int idx       = shm->q_idx[shm->q_head];
        shm->q_head   = (shm->q_head + 1) % ASYNC_QUEUE_DEPTH;
        shm->q_count--;

        shm->slots[idx].state = SLOT_RUNNING;

        /* Hand off to async_func_thread */
        shm->dispatch_slot = idx;
        shm->func_busy     = true;
        pthread_cond_signal(&shm->cond_func);

        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * async_func_thread — executor
 * ---------------------------------------------------------------------- */

/**
 * @brief Executor thread: runs the function stored in the dispatched slot.
 *
 * Waits on @c cond_func until @c dispatch_slot >= 0.  Then:
 *  1. Clears @c dispatch_slot (-1) and snapshots all request fields under
 *     the lock (so the dispatcher can proceed immediately).
 *  2. Releases the lock and calls the function with the copied parameters.
 *  3. Re-acquires the lock, writes the result into the caller's buffer
 *     (if non-NULL), marks the slot SLOT_DONE, clears @c func_busy, and
 *     broadcasts @c cond_done + signals @c cond_worker.
 *
 * @param[in] arg  Pointer to @c async_shm_t.
 * @return Always @c NULL.
 */
static void *async_func_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        pthread_mutex_lock(&shm->mutex);

        while (shm->dispatch_slot < 0 && !shm->shutdown)
            pthread_cond_wait(&shm->cond_func, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        /* Claim the dispatched slot */
        int idx        = shm->dispatch_slot;
        shm->dispatch_slot = -1;

        /* Snapshot request fields while the mutex is held */
        async_fn_t   func    = shm->slots[idx].func_ptr;
        int          np      = shm->slots[idx].num_params;
        struct iovec lparams[ASYNC_MAX_PARAMS];
        memcpy(lparams, shm->slots[idx].params, (size_t)np * sizeof(struct iovec));
        struct iovec *caller_res = shm->slots[idx].caller_result;

        pthread_mutex_unlock(&shm->mutex);

        /* Execute outside the lock so callers and the dispatcher remain free */
        struct iovec local_result = {
            .iov_base = shm->slots[idx].result_data,
            .iov_len  = ASYNC_MAX_DATA_SIZE
        };
        func(lparams, np, &local_result);

        /* Publish result */
        pthread_mutex_lock(&shm->mutex);

        if (caller_res != NULL) {
            size_t copy = local_result.iov_len;
            if (copy > caller_res->iov_len)
                copy = caller_res->iov_len;
            memcpy(caller_res->iov_base, shm->slots[idx].result_data, copy);
            caller_res->iov_len = copy;
        }

        shm->slots[idx].state = SLOT_DONE;
        shm->func_busy        = false;

        /* Wake all async_worker_wait() callers and the dispatcher */
        pthread_cond_broadcast(&shm->cond_done);
        pthread_cond_signal(&shm->cond_worker);

        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Internal helper: find a slot by request ID (caller must hold the mutex)
 * ---------------------------------------------------------------------- */

/**
 * @brief Search the slot pool for the slot whose ID matches @p id.
 *
 * @param[in] shm  Shared-memory block.
 * @param[in] id   Request ID to look up.
 * @return Slot index (0..ASYNC_QUEUE_DEPTH-1), or -1 if not found.
 */
static int find_slot(const async_shm_t *shm, async_req_id_t id)
{
    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        if (shm->slots[i].state != SLOT_FREE && shm->slots[i].id == id)
            return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the async subsystem.
 *
 * Maps shared memory, initialises synchronisation objects with
 * PTHREAD_PROCESS_SHARED, and spawns async_worker_thread and async_func_thread.
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
    shm->dispatch_slot = -1;   /* -1 = nothing pending */
    shm->next_id       = 0;    /* will be pre-incremented before first use */

    /* Mutex */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    int rc = pthread_mutex_init(&shm->mutex, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc != 0) goto err_munmap;

    /* Condition variables */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    if (pthread_cond_init(&shm->cond_worker, &ca) != 0 ||
        pthread_cond_init(&shm->cond_func,   &ca) != 0 ||
        pthread_cond_init(&shm->cond_done,   &ca) != 0) {
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
        pthread_cond_broadcast(&shm->cond_worker);
        pthread_mutex_unlock(&shm->mutex);
        pthread_join(worker->worker_tid, NULL);
        goto err_conds;
    }

    return worker;

err_conds:
    pthread_cond_destroy(&shm->cond_done);
    pthread_cond_destroy(&shm->cond_func);
    pthread_cond_destroy(&shm->cond_worker);
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
 * @param[in] worker  Handle from async_worker_init().  @c NULL is a no-op.
 */
void async_worker_destroy(async_worker_t *worker)
{
    if (!worker)
        return;

    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = true;
    pthread_cond_broadcast(&shm->cond_worker);
    pthread_cond_broadcast(&shm->cond_func);
    pthread_mutex_unlock(&shm->mutex);

    pthread_join(worker->worker_tid, NULL);
    pthread_join(worker->func_tid,   NULL);

    pthread_cond_destroy(&shm->cond_done);
    pthread_cond_destroy(&shm->cond_func);
    pthread_cond_destroy(&shm->cond_worker);
    pthread_mutex_destroy(&shm->mutex);

    munmap(shm, sizeof(*shm));
    free(worker);
}

/**
 * @brief Submit a function for asynchronous execution.
 *
 * Claims a free slot from the pool, deep-copies all request data into shared
 * memory, appends the slot index to the FIFO queue, and signals
 * async_worker_thread.  Returns immediately with a non-zero request ID.
 *
 * @param[in]  worker   Handle from async_worker_init().
 * @param[in]  func     Function to execute; must match async_fn_t.
 * @param[in]  params   Input parameter descriptors; data is deep-copied.
 * @param[in]  nparams  Number of entries in @p params (0..ASYNC_MAX_PARAMS).
 * @param[out] result   Caller-allocated output iovec.  Populated when the
 *                      matching async_worker_wait() returns.  May be @c NULL.
 *
 * @return Non-zero @c async_req_id_t on success, @c ASYNC_REQ_INVALID if the
 *         queue is full (all @c ASYNC_QUEUE_DEPTH slots are occupied).
 */
async_req_id_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams,
                           struct iovec   *result)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < ASYNC_QUEUE_DEPTH; i++) {
        if (shm->slots[i].state == SLOT_FREE) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || shm->q_count >= ASYNC_QUEUE_DEPTH) {
        pthread_mutex_unlock(&shm->mutex);
        return ASYNC_REQ_INVALID;
    }

    /* Assign a unique request ID (skip 0) */
    if (++shm->next_id == ASYNC_REQ_INVALID)
        ++shm->next_id;
    async_req_id_t id = shm->next_id;

    /* Fill slot */
    async_slot_t *slot  = &shm->slots[idx];
    slot->id            = id;
    slot->state         = SLOT_QUEUED;
    slot->func_ptr      = func;
    slot->num_params    = nparams;
    slot->caller_result = result;

    /* Deep-copy parameter data into inline storage */
    for (int i = 0; i < nparams; i++) {
        size_t sz = params[i].iov_len;
        if (sz > PARAM_SLOT_SIZE)
            sz = PARAM_SLOT_SIZE;
        memcpy(slot->param_data[i], params[i].iov_base, sz);
        slot->params[i].iov_base = slot->param_data[i];
        slot->params[i].iov_len  = sz;
    }

    /* Append to FIFO queue */
    shm->q_idx[shm->q_tail] = idx;
    shm->q_tail  = (shm->q_tail + 1) % ASYNC_QUEUE_DEPTH;
    shm->q_count++;

    pthread_cond_signal(&shm->cond_worker);
    pthread_mutex_unlock(&shm->mutex);

    return id;
}

/**
 * @brief Query the execution status of a specific request (non-blocking).
 *
 * @param[in] worker  Handle from async_worker_init().
 * @param[in] id      Request ID returned by async_func().
 *
 * @retval ASYNC_STATUS_BUSY  Slot found in QUEUED or RUNNING state.
 * @retval ASYNC_STATUS_FREE  Slot found in DONE state, or ID not found
 *                            (already released or never valid).
 */
async_status_t async_worker_status(async_worker_t *worker, async_req_id_t id)
{
    async_shm_t   *shm = worker->shm;
    async_status_t st  = ASYNC_STATUS_FREE; /* default: not found = done */

    pthread_mutex_lock(&shm->mutex);

    int idx = find_slot(shm, id);
    if (idx >= 0)
        st = (shm->slots[idx].state == SLOT_DONE) ? ASYNC_STATUS_FREE
                                                   : ASYNC_STATUS_BUSY;

    pthread_mutex_unlock(&shm->mutex);
    return st;
}

/**
 * @brief Block until a specific request is complete, then release its slot.
 *
 * Waits on @c cond_done until the slot associated with @p id reaches
 * SLOT_DONE.  The result buffer passed to async_func() is fully populated on
 * return.  The slot is then set back to SLOT_FREE; subsequent calls with the
 * same ID return immediately.
 *
 * @param[in] worker  Handle from async_worker_init().
 * @param[in] id      Request ID returned by async_func().
 */
void async_worker_wait(async_worker_t *worker, async_req_id_t id)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);

    for (;;) {
        int idx = find_slot(shm, id);
        if (idx < 0)
            break;  /* Not found: already released or never submitted */

        if (shm->slots[idx].state == SLOT_DONE) {
            /* Release the slot back to the free pool */
            shm->slots[idx].state = SLOT_FREE;
            shm->slots[idx].id    = 0;
            break;
        }

        pthread_cond_wait(&shm->cond_done, &shm->mutex);
    }

    pthread_mutex_unlock(&shm->mutex);
}
