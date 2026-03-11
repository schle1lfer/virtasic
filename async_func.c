/**
 * @file async_func.c
 * @brief Non-blocking async dispatcher implementation with a request queue.
 *
 * Two persistent threads operate on a shared-memory block (async_shm_t)
 * allocated with mmap(MAP_ANONYMOUS | MAP_SHARED):
 *
 *  async_worker_thread  (dispatcher)
 *    Dequeues the next SLOT_QUEUED slot when async_func_thread is idle,
 *    marks it SLOT_RUNNING, and signals async_func_thread via cond_func.
 *
 *  async_func_thread  (executor)
 *    Executes the function stored in the dispatched slot outside the mutex,
 *    stores the result inline in the slot, marks it SLOT_DONE, and signals
 *    cond_worker so the dispatcher can pick up the next queued item.
 *
 * Callers never block:
 *  - async_func()         enqueues and returns a request ID immediately.
 *  - async_worker_status() reads the slot state under the mutex and returns.
 *  - async_get_result()   copies inline result data and releases the slot;
 *                          returns -1 (not ready) if the slot is not DONE.
 *
 * Condition-variable map (two variables; cond_done removed)
 * ---------------------------------------------------------
 *  cond_worker  → async_worker_thread: new slot enqueued, or executor idle.
 *  cond_func    → async_func_thread:   dispatcher has loaded dispatch_slot.
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
    SLOT_FREE    = 0, /**< Available for a new submission.                  */
    SLOT_QUEUED  = 1, /**< Enqueued; waiting for async_func_thread.         */
    SLOT_RUNNING = 2, /**< Currently executing in async_func_thread.        */
    SLOT_DONE    = 3  /**< Complete; result stored inline, awaiting pickup.  */
} slot_state_t;

/**
 * @brief One entry in the request queue.
 *
 * All parameter and result data are stored inline so that no external
 * allocations are needed after the slot is filled by async_func().
 * The result is kept in the slot until the caller retrieves it with
 * async_get_result(), which then releases the slot back to SLOT_FREE.
 */
typedef struct {
    async_req_id_t  id;                                  /**< Unique request ID         */
    slot_state_t    state;                               /**< Current lifecycle state   */

    /* --- request fields (written by async_func, read by executor) ------ */
    async_fn_t      func_ptr;                            /**< Function to execute       */
    int             num_params;                          /**< Valid entries in params[] */
    struct iovec    params[ASYNC_MAX_PARAMS];            /**< Parameter descriptors     */
    uint8_t         param_data[ASYNC_MAX_PARAMS]         /**< Inline parameter bytes    */
                              [PARAM_SLOT_SIZE];

    /* --- result fields (written by executor, read by async_get_result) - */
    size_t          result_len;                          /**< Bytes written by function */
    uint8_t         result_data[ASYNC_MAX_DATA_SIZE];    /**< Inline result bytes       */
} async_slot_t;

/* -------------------------------------------------------------------------
 * Shared-memory block
 * ---------------------------------------------------------------------- */

/**
 * @brief Entire shared state visible to the caller thread,
 *        async_worker_thread, and async_func_thread.
 */
typedef struct {
    /* --- slot pool ----------------------------------------------------- */
    async_slot_t    slots[ASYNC_QUEUE_DEPTH];

    /* --- FIFO dispatch queue (holds slot indices) ----------------------- */
    int             q_idx[ASYNC_QUEUE_DEPTH];
    int             q_head;   /**< Dequeue position                         */
    int             q_tail;   /**< Enqueue position                         */
    int             q_count;  /**< Current queue depth                      */

    /* --- request ID generator ------------------------------------------ */
    async_req_id_t  next_id;  /**< Incremented before each use; 0 skipped   */

    /* --- dispatcher → executor handoff --------------------------------- */
    int             dispatch_slot; /**< Slot index to execute; -1 = none    */

    /* --- executor state ------------------------------------------------ */
    bool            func_busy; /**< TRUE while async_func_thread is running  */

    /* --- lifecycle ------------------------------------------------------ */
    bool            shutdown;

    /* --- POSIX synchronisation (PTHREAD_PROCESS_SHARED) ---------------- */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_worker; /**< → async_worker_thread (enqueue/idle)  */
    pthread_cond_t  cond_func;   /**< → async_func_thread   (dispatch)      */
} async_shm_t;

/**
 * @brief Opaque handle definition.
 */
struct async_worker {
    async_shm_t *shm;
    pthread_t    worker_tid; /**< async_worker_thread (dispatcher) */
    pthread_t    func_tid;   /**< async_func_thread   (executor)   */
};

/* -------------------------------------------------------------------------
 * async_worker_thread — dispatcher
 * ---------------------------------------------------------------------- */

/**
 * @brief Dispatcher thread: feeds queued slots to async_func_thread one at a
 *        time.
 *
 * Waits on @c cond_worker until the FIFO is non-empty AND async_func_thread
 * is idle AND no dispatch is already pending.  Dequeues the head slot, marks
 * it SLOT_RUNNING, writes its index to @c dispatch_slot, and signals
 * @c cond_func.
 *
 * @param[in] arg  Pointer to @c async_shm_t.
 * @return Always @c NULL.
 */
static void *async_worker_thread(void *arg)
{
    async_shm_t *shm = (async_shm_t *)arg;

    for (;;) {
        pthread_mutex_lock(&shm->mutex);

        while (!shm->shutdown &&
               (shm->q_count == 0 || shm->func_busy || shm->dispatch_slot >= 0))
            pthread_cond_wait(&shm->cond_worker, &shm->mutex);

        if (shm->shutdown) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }

        int idx     = shm->q_idx[shm->q_head];
        shm->q_head = (shm->q_head + 1) % ASYNC_QUEUE_DEPTH;
        shm->q_count--;

        shm->slots[idx].state = SLOT_RUNNING;
        shm->dispatch_slot    = idx;
        shm->func_busy        = true;

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
 *  1. Waits on @c cond_func until @c dispatch_slot >= 0.
 *  2. Claims the slot index, clears @c dispatch_slot, snapshots all request
 *     fields, and releases the mutex.
 *  3. Calls the function outside the lock.
 *  4. Re-acquires the mutex, stores the result inline in
 *     @c slots[idx].result_data / result_len, marks the slot SLOT_DONE,
 *     clears @c func_busy, and signals @c cond_worker so the dispatcher can
 *     dequeue the next item.
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

        int idx            = shm->dispatch_slot;
        shm->dispatch_slot = -1;

        /* Snapshot request under the lock */
        async_fn_t   func = shm->slots[idx].func_ptr;
        int          np   = shm->slots[idx].num_params;
        struct iovec lparams[ASYNC_MAX_PARAMS];
        memcpy(lparams, shm->slots[idx].params, (size_t)np * sizeof(struct iovec));

        pthread_mutex_unlock(&shm->mutex);

        /* Execute outside the lock */
        struct iovec local_result = {
            .iov_base = shm->slots[idx].result_data,
            .iov_len  = ASYNC_MAX_DATA_SIZE
        };
        func(lparams, np, &local_result);

        /* Store result inline and mark slot DONE */
        pthread_mutex_lock(&shm->mutex);

        shm->slots[idx].result_len = local_result.iov_len;
        shm->slots[idx].state      = SLOT_DONE;
        shm->func_busy             = false;

        /* Wake the dispatcher so it can dequeue the next item */
        pthread_cond_signal(&shm->cond_worker);

        pthread_mutex_unlock(&shm->mutex);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Internal: find slot by ID (caller must hold the mutex)
 * ---------------------------------------------------------------------- */

/**
 * @brief Search the slot pool for a slot matching @p id.
 *
 * @param[in] shm  Shared-memory block.
 * @param[in] id   Request ID to look up.
 * @return Slot index on success, -1 if not found.
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
 * Maps shared memory, initialises two condition variables and one mutex with
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
    shm->dispatch_slot = -1;

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
        pthread_cond_init(&shm->cond_func,   &ca) != 0) {
        pthread_condattr_destroy(&ca);
        goto err_mutex;
    }
    pthread_condattr_destroy(&ca);

    worker->shm = shm;

    if (pthread_create(&worker->worker_tid, NULL, async_worker_thread, shm) != 0)
        goto err_conds;

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

    pthread_cond_destroy(&shm->cond_func);
    pthread_cond_destroy(&shm->cond_worker);
    pthread_mutex_destroy(&shm->mutex);
    munmap(shm, sizeof(*shm));
    free(worker);
}

/**
 * @brief Submit a function for asynchronous execution (non-blocking).
 *
 * Finds a free slot, deep-copies all request data into shared memory, appends
 * the slot to the FIFO, and returns a unique request ID immediately.
 *
 * @param[in] worker   Handle from async_worker_init().
 * @param[in] func     Function to execute; must match async_fn_t.
 * @param[in] params   Input parameter descriptors; data is deep-copied.
 * @param[in] nparams  Number of entries in @p params (0..ASYNC_MAX_PARAMS).
 *
 * @return Non-zero @c async_req_id_t on success, @c ASYNC_REQ_INVALID if all
 *         @c ASYNC_QUEUE_DEPTH slots are currently occupied.
 */
async_req_id_t async_func(async_worker_t *worker,
                           async_fn_t      func,
                           struct iovec   *params,
                           int             nparams)
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
    async_slot_t *slot = &shm->slots[idx];
    slot->id           = id;
    slot->state        = SLOT_QUEUED;
    slot->func_ptr     = func;
    slot->num_params   = nparams;
    slot->result_len   = 0;

    for (int i = 0; i < nparams; i++) {
        size_t sz = params[i].iov_len;
        if (sz > PARAM_SLOT_SIZE)
            sz = PARAM_SLOT_SIZE;
        memcpy(slot->param_data[i], params[i].iov_base, sz);
        slot->params[i].iov_base = slot->param_data[i];
        slot->params[i].iov_len  = sz;
    }

    /* Enqueue */
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
 * @retval ASYNC_STATUS_BUSY  Slot is in QUEUED or RUNNING state.
 * @retval ASYNC_STATUS_FREE  Slot is DONE (result ready) or ID not found.
 */
async_status_t async_worker_status(async_worker_t *worker, async_req_id_t id)
{
    async_shm_t   *shm = worker->shm;
    async_status_t st  = ASYNC_STATUS_FREE;

    pthread_mutex_lock(&shm->mutex);

    int idx = find_slot(shm, id);
    if (idx >= 0 && shm->slots[idx].state != SLOT_DONE)
        st = ASYNC_STATUS_BUSY;

    pthread_mutex_unlock(&shm->mutex);
    return st;
}

/**
 * @brief Retrieve the result of a completed request and release its slot.
 *
 * Non-blocking.  Copies the inline result data into the caller's buffer and
 * sets the slot back to SLOT_FREE.  Fails immediately if the request is not
 * yet in the DONE state.
 *
 * @param[in]  worker  Handle from async_worker_init().
 * @param[in]  id      Request ID returned by async_func().
 * @param[out] result  Caller-allocated iovec.  On success iov_base receives
 *                     the function's output and iov_len is updated to the
 *                     number of bytes copied (≤ original iov_len).
 *                     May be @c NULL if the return value is not needed.
 *
 * @retval  0   Success; result copied and slot released.
 * @retval -1   Slot not found, or not yet in DONE state.
 */
int async_get_result(async_worker_t *worker,
                     async_req_id_t  id,
                     struct iovec   *result)
{
    async_shm_t *shm = worker->shm;

    pthread_mutex_lock(&shm->mutex);

    int idx = find_slot(shm, id);
    if (idx < 0 || shm->slots[idx].state != SLOT_DONE) {
        pthread_mutex_unlock(&shm->mutex);
        return -1;
    }

    /* Copy inline result into the caller's buffer */
    if (result != NULL) {
        size_t copy = shm->slots[idx].result_len;
        if (copy > result->iov_len)
            copy = result->iov_len;
        memcpy(result->iov_base, shm->slots[idx].result_data, copy);
        result->iov_len = copy;
    }

    /* Release slot back to the free pool */
    shm->slots[idx].state = SLOT_FREE;
    shm->slots[idx].id    = 0;

    pthread_mutex_unlock(&shm->mutex);
    return 0;
}
