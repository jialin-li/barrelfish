/*
 * Copyright (c) 2010, 2011, 2012, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
 * Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/spawn_client.h>

#include <flounder/flounder_txqueue.h>
#include <spawndomain/spawndomain.h>

#include <xeon_phi/xeon_phi.h>
#include <xeon_phi/xeon_phi_client.h>
#include <xeon_phi/xeon_phi_domain.h>

#include <bomp_internal.h>
#include <xomp/xomp.h>

#include <if/xomp_defs.h>

#include <xomp_debug.h>

/**
 * \brief worker state enumeration.
 *
 * Describes the possible states a worker can be in.
 */
typedef enum xomp_worker_state
{
    XOMP_WORKER_ST_INVALID  = 0,    ///< this worker has not been initialized
    XOMP_WORKER_ST_FAILURE  = 1,    ///< an error occurred during an operation
    XOMP_WORKER_ST_SPAWNING = 2,    ///< worker is being spawned
    XOMP_WORKER_ST_SPAWNED  = 3,    ///< worker is spawned and connected to master
    XOMP_WORKER_ST_READY    = 4,    ///< worker is ready to service requests
    XOMP_WORKER_ST_BUSY     = 5     ///< worker is busy servicing requests
} xomp_worker_st_t;

/**
 * \brief worker type enumeration
 *
 * Describes the possible worker types i.e. where the domain is running on
 */
typedef enum xomp_worker_type
{
    XOMP_WORKER_TYPE_INVALID = 0,  ///< invalid worker type (not initialized)
    XOMP_WORKER_TYPE_LOCAL   = 1,  ///< worker runs local to master
    XOMP_WORKER_TYPE_REMOTE  = 2   ///< worker runs remote to master
} xomp_worker_type_t;

/**
 * \brief XOMP worker
 */
struct xomp_worker
{
    xomp_wid_t id;                  ///< worker ID
    xomp_worker_type_t type;        ///< worker type
    xomp_worker_st_t state;         ///< worker state
    xphi_dom_id_t domainid;         ///< domain ID of the worker

    struct xomp_binding *binding;   ///< Control channel binding
    struct tx_queue txq;            ///< Flounder TX queue

    errval_t err;                   ///< error number in case an error occurred
    uint8_t add_mem_st;             ///< state flag when we adding a frame

    struct capref msgframe;         ///< messaging frame + tls for the worker
    lpaddr_t msgbase;               ///< physical base of the messaging frame
    void *msgbuf;                   ///< where the messaging frame is mapped

    void *tls;                      ///< pointer to the thread local storage
};

/**
 * \brief XOMP master
 */
struct xomp_master
{
    uint32_t numworker;                 ///< total number of worker spawned
    struct {
        uint32_t num;                   ///< number of local workers
        struct xomp_worker *workers;    ///< array of local workers
        uint32_t next;                  ///< next local worker to "allocate"
    } local;
    struct {
        uint32_t num;                   ///< number of remote workers
        struct xomp_worker *workers;    ///< array of remote workers
        uint32_t next;                  ///< next remote worker to "allocate"
    } remote;
};

/**
 * \brief Message state for the TX queue
 */
struct xomp_msg_st
{
    struct txq_msg_st common;       ///< common msg state
    /* union of arguments */
    union {
        struct {
            uint64_t fn;
            uint64_t arg;
            uint64_t id;
            uint64_t flags;
        } do_work;
        struct {
            struct capref frame;
            uint64_t vaddr;
            uint8_t type;
        } add_mem;
    } args;
};

/// intialized flag
static uint8_t xomp_master_initialized = 0x0;

/// XOMP master
static struct xomp_master xmaster;

/// exported service iref (for local workers)
static iref_t svc_iref;

/// number of present Xeon Phis
static uint8_t num_phi = 0;

/// only use remote workers, no locals
static xomp_wloc_t worker_loc = XOMP_WORKER_LOC_MIXED;

/// stride for core allocation (in case of hyperthreads)
static coreid_t core_stride;

/// arguments to supply to the local spawned workers
static struct xomp_spawn spawn_args_local;

/// arguments to supply to the remote spawned workers
static struct xomp_spawn spawn_args_remote;

/// buffer for the worker id argument
char worker_id_buf[26];

/// buffer for the iref argument
char iref_buf[19];

/**
 * \brief enters the barrier when a worker finished his work, this function
 *        is called on the main thread (master domain)
 *
 * \param barrier   The barrier to enter
 */
static inline void xbomp_barrier_enter_no_wait(struct bomp_barrier *barrier)
{
    if (__sync_fetch_and_add(&barrier->counter, 1) == (barrier->max - 1)) {
        barrier->counter = 0;
        barrier->cycle = !barrier->cycle;
    }
}

/*
 * ----------------------------------------------------------------------------
 * Helper functions
 * ----------------------------------------------------------------------------
 */
static inline uint32_t xomp_master_get_local_threads(uint32_t nthreads)
{
    switch (worker_loc) {
        case XOMP_WORKER_LOC_LOCAL:
            return nthreads - 1;
        case XOMP_WORKER_LOC_MIXED:
            return nthreads - (num_phi * ((nthreads) / (1 + num_phi))) - 1;
        case XOMP_WORKER_LOC_REMOTE:
            return 0;
        default:
            USER_PANIC("unknown worker location!");
    }
    USER_PANIC("unknown worker location!");
    return 0;
}

static inline uint32_t xomp_master_get_remote_threads(uint32_t nthreads)
{
    switch (worker_loc) {
        case XOMP_WORKER_LOC_LOCAL:
            return 0;
        case XOMP_WORKER_LOC_MIXED:
            return ((nthreads) / (1 + num_phi)) * num_phi;
        case XOMP_WORKER_LOC_REMOTE:
            return nthreads - 1;
        default:
            USER_PANIC("unknown worker location!");
    }

    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * XOMP channel send handlers
 * ----------------------------------------------------------------------------
 */

static errval_t do_work_tx(struct txq_msg_st *msg_st)
{

    struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;

    return xomp_do_work__tx(msg_st->queue->binding, TXQCONT(msg_st),
                            st->args.do_work.fn, st->args.do_work.arg,
                            st->args.do_work.id, st->args.do_work.flags);
}

static errval_t gw_req_memory_call_tx(struct txq_msg_st *msg_st)
{

    struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;

    return xomp_gw_req_memory_call__tx(msg_st->queue->binding, TXQCONT(msg_st),
                                       st->args.add_mem.vaddr,
                                       st->args.add_mem.type);
}

static errval_t add_memory_call_tx(struct txq_msg_st *msg_st)
{

    struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;

    return xomp_add_memory_call__tx(msg_st->queue->binding, TXQCONT(msg_st),
                                    st->args.add_mem.frame,
                                    st->args.add_mem.vaddr, st->args.add_mem.type);
}

/*
 * ----------------------------------------------------------------------------
 * XOMP channel receive handlers
 * ----------------------------------------------------------------------------
 */

static void gw_req_memory_response_rx(struct xomp_binding *b,
                                      errval_t msgerr)
{
    XMP_DEBUG("gw_req_memory_response_rx: %s\n", err_getstring(msgerr));

    struct xomp_worker *worker = b->st;

    worker->err = msgerr;
    worker->add_mem_st = 0x2;
}

static void add_memory_response_rx(struct xomp_binding *b,
                                   errval_t msgerr)
{
    XMP_DEBUG("add_memory_response_rx: %s\n", err_getstring(msgerr));

    struct xomp_worker *worker = b->st;

    worker->err = msgerr;
    worker->add_mem_st = 0x2;
}

static inline void done_msg_common(struct xomp_binding *b,
                                   uint64_t tid,
                                   errval_t msgerr)
{
    struct xomp_task *task = (struct xomp_task *) tid;

    struct xomp_worker *worker = b->st;
    if (err_is_fail(msgerr)) {
        worker->state = XOMP_WORKER_ST_FAILURE;
    } else {
        worker->state = XOMP_WORKER_ST_READY;
    }

    xbomp_barrier_enter_no_wait(task->barrier);

    /* if the last worker returns, free up the task data structure */
    task->done++;
    if (task->done == task->total_threads) {
        free(task);
    }
}

static void done_with_arg_rx(struct xomp_binding *b,
                             uint64_t tid,
                             uint64_t arg,
                             errval_t msgerr)
{
    XMP_DEBUG("done_with_arg_rx: arg:%lx, id:%lx\n", arg, tid);

    done_msg_common(b, tid, msgerr);

    /* XXX: do something with the argument */
}

static void done_notify_rx(struct xomp_binding *b,
                           uint64_t tid,
                           errval_t msgerr)
{
    XMP_DEBUG("done_notify_rx: id:%lx\n", tid);

    done_msg_common(b, tid, msgerr);
}

static struct xomp_rx_vtbl rx_vtbl = {
    .gw_req_memory_response = gw_req_memory_response_rx,
    .add_memory_response = add_memory_response_rx,
    .done_notify = done_notify_rx,
    .done_with_arg = done_with_arg_rx
};

/*
 * ----------------------------------------------------------------------------
 * XOMP channel connect handler
 * ----------------------------------------------------------------------------
 */

static errval_t xomp_svc_connect_cb(void *st,
                                    struct xomp_binding *xb)
{
    struct xomp_worker *worker = xmaster.local.workers + xmaster.local.next++;

    XMI_DEBUG("xomp_svc_connect_cb:%lx connected: %p\n", worker->id, worker);

    xb->rx_vtbl = rx_vtbl;
    xb->st = worker;

    txq_init(&worker->txq, xb, xb->waitset, (txq_register_fn_t) xb->register_send,
             sizeof(struct xomp_msg_st));

    worker->binding = xb;
    worker->state = XOMP_WORKER_ST_SPAWNED;

    return SYS_ERR_OK;
}

static void xomp_svc_export_cb(void *st,
                               errval_t err,
                               iref_t iref)
{
    XMI_DEBUG("Service exported @ iref:%"PRIuIREF", %s\n", iref, err_getstring(err));

    svc_iref = iref;
}

/**
 * \brief XOMP channel connect callback called by the Flounder backend
 *
 * \param st    Supplied worker state
 * \param err   outcome of the connect attempt
 * \param xb    XOMP Flounder binding
 */
static void worker_connect_cb(void *st,
                              errval_t err,
                              struct xomp_binding *xb)
{

    struct xomp_worker *worker = st;

    XMI_DEBUG("worker:%lx connected: %s\n", worker->id, err_getstring(err));

    if (err_is_fail(err)) {
        worker->state = XOMP_WORKER_ST_FAILURE;
        return;
    }

    xb->rx_vtbl = rx_vtbl;
    xb->st = worker;

    txq_init(&worker->txq, xb, xb->waitset, (txq_register_fn_t) xb->register_send,
             sizeof(struct xomp_msg_st));

    worker->binding = xb;
    worker->state = XOMP_WORKER_ST_SPAWNED;
}

/*
 * ============================================================================
 * Public Interface
 * ============================================================================
 */

/**
 * \brief initializes the Xeon Phi openMP library
 *
 * \param args struct containing the master initialization values
 *
 * \returns SYS_ERR_OK on success
 *          errval on failure
 */

errval_t xomp_master_init(struct xomp_args *args)
{
    errval_t err;

    if (xomp_master_initialized) {
        XMI_DEBUG("WARNIG: XOMP master already initialized\n");
        return SYS_ERR_OK;
    }

    if (args->type == XOMP_ARG_TYPE_WORKER) {
        return -1;  // TODO: ERRNO
    }

    if (args->core_stride != 0) {
        core_stride = args->core_stride;
    } else {
        core_stride = BOMP_DEFAULT_CORE_STRIDE;
    }

    if (args->type == XOMP_ARG_TYPE_UNIFORM) {
        num_phi = args->args.uniform.nphi;
        worker_loc = args->args.uniform.worker_loc;
    } else {
        num_phi = args->args.distinct.nphi;

        worker_loc = args->args.distinct.worker_loc;
    }

    XMI_DEBUG("Initializing XOMP master with nthreads:%u, nphi:%u\n",
              args->args.uniform.nthreads, args->args.uniform.nphi);

    /* exporting the interface for local workers */
    err = xomp_export(NULL, xomp_svc_export_cb, xomp_svc_connect_cb,
                      get_default_waitset(), IDC_EXPORT_FLAGS_DEFAULT);
    if (err_is_fail(err)) {
        return err;
    }

    while (svc_iref == 0) {
        err = event_dispatch(get_default_waitset());
        if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "event dispatch\n");
        }
    }

    char **argv = NULL;

    if (args->type == XOMP_ARG_TYPE_UNIFORM) {

        spawn_args_local.argc = args->args.uniform.argc;
        spawn_args_remote.argc = args->args.uniform.argc;

        err = xomp_master_build_path(&spawn_args_local.path, &spawn_args_remote.path);
        if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "could not build the path");
        }
        argv = args->args.uniform.argv;
    } else {
        spawn_args_local.argc = args->args.distinct.local.argc;
        spawn_args_local.path = args->args.distinct.local.path;
        spawn_args_remote.path = args->args.distinct.remote.path;
        argv = args->args.distinct.local.argv;
    }

    spawn_args_local.argv = calloc(spawn_args_local.argc + 3, sizeof(char *));

    if (spawn_args_local.argv == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }

    for (uint8_t i = 0; i < spawn_args_local.argc; ++i) {
        spawn_args_local.argv[i] = argv[i];
    }

    spawn_args_local.argv[spawn_args_local.argc++] = XOMP_WORKER_ARG;
    spawn_args_local.argv[spawn_args_local.argc++] = worker_id_buf;
    spawn_args_local.argv[spawn_args_local.argc++] = iref_buf;
    spawn_args_local.argv[spawn_args_local.argc] = NULL;

    snprintf(iref_buf, sizeof(iref_buf), "--iref=0x%08x", svc_iref);

    /* remote initialization */

    if (args->type == XOMP_ARG_TYPE_DISTINCT) {
        argv = args->args.distinct.remote.argv;
        spawn_args_remote.argc = args->args.distinct.remote.argc;
    }

    spawn_args_remote.argv = calloc(spawn_args_remote.argc + 2, sizeof(char *));

    if (spawn_args_remote.argv == NULL) {
        free(spawn_args_local.argv);
        return LIB_ERR_MALLOC_FAIL;
    }

    for (uint8_t i = 0; i < spawn_args_remote.argc; ++i) {
        spawn_args_remote.argv[i] = argv[i];
    }

    spawn_args_remote.argv[spawn_args_remote.argc++] = XOMP_WORKER_ARG;
    spawn_args_remote.argv[spawn_args_remote.argc++] = worker_id_buf;
    spawn_args_remote.argv[spawn_args_remote.argc] = NULL;

    xomp_master_initialized = 0x1;

    return SYS_ERR_OK;
}

/**
 * \brief Spawns the worker threads on the Xeon Phi
 *
 * \param nworkers    Number of total workers this includes the Master
 *
 * \returns SYS_ERR_OK on success
 *          errval on failure
 */
errval_t xomp_master_spawn_workers(uint32_t nworkers)
{
    errval_t err;

    if (!xomp_master_initialized) {
        return XOMP_ERR_MASTER_NOT_INIT;
    }

    xmaster.numworker = nworkers;

    struct xomp_worker *workers = calloc(nworkers, sizeof(struct xomp_worker));

    if (workers == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }

    uint32_t remote_threads = xomp_master_get_remote_threads(nworkers);
    uint32_t local_threads = xomp_master_get_local_threads(nworkers);

    xmaster.local.next = 0;
    xmaster.remote.next = 0;
    xmaster.local.num = local_threads;
    xmaster.remote.num = remote_threads;
    xmaster.local.workers = workers;

    if (remote_threads > 0) {
        err = spawn_symval_cache_init(0);
        if (err_is_fail(err)) {
            return err;
        }
    }

    if (num_phi > 0) {
        xmaster.remote.workers = workers + local_threads;
    }

    XMI_DEBUG("spawning %u workers: local:%u, remote: %ux%u\n", nworkers - 1,
              local_threads, num_phi,
              (num_phi != 0 ? remote_threads / num_phi : remote_threads));

    assert((remote_threads + local_threads + 1) == nworkers);

    xphi_id_t xid = 0;
    coreid_t core = disp_get_core_id() + core_stride;

    for (uint32_t i = 0; i < remote_threads + local_threads; ++i) {
#ifdef __k1om__
        if (xid == disp_xeon_phi_id()) {
            xid = (xid + 1) % num_phi;
        }
#endif
        if (i == local_threads) {
            core = 0;
        }

        struct xomp_worker *worker = workers + i;

#ifndef __k1om__
        /*
         * XXX: we have to set the ram affinity in order to have a higher chance
         *      the node gets found at the Xeon Phi. It may be split up otherwise
         */
        uint64_t min_base, max_limit;
        ram_get_affinity(&min_base, &max_limit);
        ram_set_affinity(XOMP_RAM_MIN_BASE, XOMP_RAM_MAX_LIMIT);
#endif

        if (i < local_threads) {
            err = frame_alloc(&worker->msgframe, XOMP_TLS_SIZE, NULL);
        } else {
            err = frame_alloc(&worker->msgframe, XOMP_FRAME_SIZE, NULL);
        }

#ifndef __k1om__
        ram_set_affinity(min_base, max_limit);
#endif

        if (err_is_fail(err)) {
            /* TODO: cleanup */
            worker->state = XOMP_WORKER_ST_FAILURE;
            debug_printf("xomp_master_spawn_workers: FAILURE, %s\n",
                         err_getstring(err));
            return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
        }

        struct frame_identity id;
        err = invoke_frame_identify(worker->msgframe, &id);
        if (err_is_fail(err)) {
            /* TODO: cleanup */
            return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
        }

        /* TODO: build a good id */
        worker->id = ((uint64_t) disp_get_domain_id()) << 32 | i;
        worker->msgbase = id.base;
        worker->state = XOMP_WORKER_ST_SPAWNING;

        err = vspace_map_one_frame(&worker->msgbuf, (1UL << id.bits),
                                   worker->msgframe, NULL, NULL);

        if (err_is_fail(err)) {
            /* TODO: cleanup */
            return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
        }

        XMI_DEBUG("messaging frame mapped: [%016lx] @ [%016lx]\n",
                  worker->msgbase, (lvaddr_t )worker->msgbuf);

        if (i < local_threads) {
            snprintf(worker_id_buf, sizeof(worker_id_buf), "--wid=%016"PRIx64,
                     worker->id);
            /*
             * TODO: set a gateway domain for each NUMA node as it is done with
             *       the Xeon Phi
             */
            worker->tls = worker->msgbuf;

            XMI_DEBUG("spawning {%s} on host, core:%u\n", spawn_args_local.path,
                      core);

            domainid_t did;
            err = spawn_program_with_caps(core, spawn_args_local.path,
                                          spawn_args_local.argv, NULL, NULL_CAP,
                                          worker->msgframe, SPAWN_FLAGS_OMP,
                                          &did);
            worker->domainid = did;
            worker->type = XOMP_WORKER_TYPE_LOCAL;
            if (err_is_fail(err)) {
                /* TODO: cleanup */
                return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
            }

            core += core_stride;
        } else {
            /*
             * we give give the first worker domains the gateway flag so it
             * initializes the gateway service while others will connect to it
             */
            if (core == 0) {
                worker->id |= XOMP_WID_GATEWAY_FLAG;
            }

            snprintf(worker_id_buf, sizeof(worker_id_buf), "--wid=%016"PRIx64,
                     worker->id);

            worker->tls = ((uint8_t *) worker->msgbuf) + XOMP_MSG_FRAME_SIZE;

            struct xomp_frameinfo fi = {
                .sendbase = worker->msgbase + XOMP_MSG_CHAN_SIZE,
                .inbuf = worker->msgbuf,
                .inbufsize = XOMP_MSG_CHAN_SIZE,
                .outbuf = ((uint8_t *) worker->msgbuf) + XOMP_MSG_CHAN_SIZE,
                .outbufsize = XOMP_MSG_CHAN_SIZE
            };

            err = xomp_accept(&fi, worker, worker_connect_cb,
                              get_default_waitset(), IDC_EXPORT_FLAGS_DEFAULT);

            if (err_is_fail(err)) {
                /* TODO: Clean up */
                return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
            }

            XMI_DEBUG("spawning {%s} on xid:%u, core:%u\n",
                      spawn_args_remote.path, xid, core);

            err = xeon_phi_client_spawn(xid, core, spawn_args_remote.path,
                                        spawn_args_remote.argv, worker->msgframe,
                                        SPAWN_FLAGS_OMP, &worker->domainid);
            if (err_is_fail(err)) {
                /* TODO: cleanup */
                return err_push(err, XOMP_ERR_SPAWN_WORKER_FAILED);
            }
            worker->type = XOMP_WORKER_TYPE_REMOTE;
            xid++; 
        }

        XMI_DEBUG("waiting for client %p to connect...\n", worker);

        while (worker->state == XOMP_WORKER_ST_SPAWNING) {
            err = event_dispatch(get_default_waitset());
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "event dispatch\n");
            }
        }

        if (worker->state == XOMP_WORKER_ST_FAILURE) {
            return XOMP_ERR_SPAWN_WORKER_FAILED;
        }

        worker->state = XOMP_WORKER_ST_READY;

        if (i >= local_threads) {
            if (xid == num_phi) {
                xid = 0;
                core++; // no stride on xeon phi
            }
        }
    }

    xmaster.local.next = 0;
    xmaster.remote.next = 0;

    return SYS_ERR_OK;
}

/**
 * \brief Adds a memory region to be used for work
 *
 * \param frame Frame to be shared
 * \param info  information about the frame i.e. virtual address to map
 * \oaram type  Type of the frame
 *
 * \returns SYS_ERR_OK on success
 *          errval on error
 */

errval_t xomp_master_add_memory(struct capref frame,
                                uint64_t info,
                                xomp_frame_type_t type)
{
    errval_t err;

    if (!xomp_master_initialized) {
        return XOMP_ERR_MASTER_NOT_INIT;
    }

    struct xomp_worker *worker;

    XMI_DEBUG("adding memory of type %u @ info: %016lx\n", type, info);

    /*
     * we adding the memory to the worker domains with the Xeon Phi Gateway
     * domains first. This is expected to take the longest time. (potential
     * replication and going through the Xeon Phi drivers).
     *
     * For subsequent worker domains, we just send the messages asynchronously
     */
    for (uint32_t i = 0; i < xmaster.remote.num; ++i) {
        worker = &xmaster.remote.workers[i];
        if (worker->id & XOMP_WID_GATEWAY_FLAG) {
            xphi_id_t xid = xeon_phi_domain_get_xid(worker->domainid);
            err = xeon_phi_client_chan_open(xid, worker->domainid, info, frame,
                                            type);
            if (err_is_fail(err)) {
                worker->state = XOMP_WORKER_ST_FAILURE;
                /*
                 * XXX: if the gateway domain fails, the entire node is not
                 *      operational.
                 */
                return err;
            }
        } else {
            assert(worker->add_mem_st == 0x0);

            worker->add_mem_st = 0x1;

            struct txq_msg_st *msg_st = txq_msg_st_alloc(&worker->txq);

            if (msg_st == NULL) {
                return LIB_ERR_MALLOC_FAIL;
            }

            msg_st->send = gw_req_memory_call_tx;
            msg_st->cleanup = NULL;

            struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;
            st->args.add_mem.vaddr = info;
            st->args.add_mem.type = type;

            txq_send(msg_st);
        }
    }

    /* send the memory caps to the local workers directly */
    for (uint32_t i = 0; i < xmaster.local.num; ++i) {
        worker = &xmaster.local.workers[i];
        assert(worker->type == XOMP_WORKER_TYPE_LOCAL);
        assert(worker->add_mem_st == 0x0);

        worker->add_mem_st = 0x1;

        struct txq_msg_st *msg_st = txq_msg_st_alloc(&worker->txq);

        if (msg_st == NULL) {
            return LIB_ERR_MALLOC_FAIL;
        }

        msg_st->send = add_memory_call_tx;
        msg_st->cleanup = NULL;

        struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;
        st->args.add_mem.frame = frame;
        st->args.add_mem.vaddr = info;
        st->args.add_mem.type = type;

        txq_send(msg_st);
    }

    /* wait for the replies */

    for (uint32_t i = 0; i < xmaster.remote.num; ++i) {
        worker = &xmaster.remote.workers[i];
        if (worker->id & XOMP_WID_GATEWAY_FLAG) {
            continue;
        }
        while (worker->add_mem_st == 0x1) {
            err = event_dispatch(get_default_waitset());
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "event dispatch\n");
            }
        }
        if (err_is_fail(worker->err)) {
            debug_printf("xomp_master_add_memory: FAILURE, %s\n",
                         err_getstring(worker->err));
            worker->state = XOMP_WORKER_ST_FAILURE;
            return worker->err;
        }
        worker->add_mem_st = 0x0;
    }

    for (uint32_t i = 0; i < xmaster.local.num; ++i) {
        worker = &xmaster.local.workers[i];
        assert(worker->type == XOMP_WORKER_TYPE_LOCAL);

        while (worker->add_mem_st == 0x1) {
            err = event_dispatch(get_default_waitset());
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "event dispatch\n");
            }
        }
        if (err_is_fail(worker->err)) {
            debug_printf("xomp_master_add_memory: FAILURE %s\n",
                         err_getstring(worker->err));
            worker->state = XOMP_WORKER_ST_FAILURE;
            return worker->err;
        }
        worker->add_mem_st = 0x0;
    }

    return SYS_ERR_OK;
}

/**
 * \brief builds the argument path based on the own binary name
 *
 * \param local  pointer where to store the local path
 * \param remote pointer where to store the remote path
 *
 * \returns SYS_ERR_OK on success
 */
errval_t xomp_master_build_path(char **local,
                                char **remote)
{
    size_t length, size = 0;

    size += snprintf(NULL, 0, "/x86_64/sbin/%s", disp_name()) + 1;
    size += snprintf(NULL, 0, "/k1om/sbin/%s", disp_name()) + 1;

    char *path = malloc(size);
    if (path == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }

    length = snprintf(path, size, "/x86_64/sbin/%s", disp_name());
    path[length] = '\0';
    size -= (++length);

    if (local) {
        *local = path;
    }

    path += length;
    length = snprintf(path, size, "/k1om/sbin/%s", disp_name());
    path[length] = '\0';

    if (remote) {
        *remote = path;
    }

    return SYS_ERR_OK;
}

/**
 * \brief executes some work on each worker domains
 *
 * \param task information about the task
 *
 * \returns SYS_ERR_OK on success
 *          errval on error
 */
errval_t xomp_master_do_work(struct xomp_task *task)
{
    errval_t err;

    if (!xomp_master_initialized) {
        return XOMP_ERR_MASTER_NOT_INIT;
    }

    uint64_t fn = 0;

    uint32_t remote_threads = xomp_master_get_remote_threads(task->total_threads);
    uint32_t local_threads = xomp_master_get_local_threads(task->total_threads);

    XMP_DEBUG("Executing task with %u workers host:%u, xphi:%ux%u]\n",
              task->total_threads, local_threads + 1, num_phi, remote_threads);

    assert(local_threads <= xmaster.local.num);
    assert(remote_threads <= xmaster.remote.num);
    assert((local_threads + remote_threads + 1) == task->total_threads);

    uint32_t fn_idx;
    char *fn_name;

    if (remote_threads > 0) {
        /*
         * do the address translation for the remote workers
         */
        err = spawn_symval_lookup_addr((genvaddr_t) task->fn, &fn_idx, &fn_name);
        if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "looking up address\n");
            return err;
        }
    }

    for (uint32_t i = task->total_threads - 1; i > 0; --i) {
        struct xomp_worker *worker = NULL;

        if (i <= local_threads) {
            worker = &xmaster.local.workers[xmaster.local.next++];
            assert(worker->type == XOMP_WORKER_TYPE_LOCAL);

            if (xmaster.local.next == xmaster.local.num) {
                xmaster.local.next = 0;
            }

            XMP_DEBUG("local worker id:%lx\n", worker->id);

            fn = (uint64_t) task->fn;

        } else {
            worker = &xmaster.remote.workers[xmaster.remote.next++];
            assert(worker->type == XOMP_WORKER_TYPE_REMOTE);
            assert(fn_idx != 0);

            if (xmaster.remote.next == xmaster.remote.num) {
                xmaster.remote.next = 0;
            }
            // build the function address based on the flag and the index
            fn = (uint64_t) fn_idx | XOMP_FN_INDEX_FLAG;

            XMP_DEBUG("remote worker id: %lx, function %s @ index %u\n",
                      worker->id, fn_name, fn_idx);

        }

        if (worker->state != XOMP_WORKER_ST_READY) {
            return XOMP_ERR_WORKER_STATE;
        }
        assert(worker->state == XOMP_WORKER_ST_READY);
        worker->state = XOMP_WORKER_ST_BUSY;

        struct bomp_work *work = worker->tls;

        work->fn = task->fn;

        work->barrier = NULL;
        work->thread_id = i;
        work->num_threads = task->total_threads;

        /* XXX: hack, we do not know how big the data section is... */
        uint64_t *src = task->arg;
        uint64_t *dst = (uint64_t *) (work + 1);
        uint32_t bytes = 0;
        while (*src != 0 || bytes < 64) {
            *dst++ = *src++;
            bytes += 8;
        }

        struct txq_msg_st *msg_st = txq_msg_st_alloc(&worker->txq);

        if (msg_st == NULL) {
            if (i == 0) {
                free(task);
            }
            return LIB_ERR_MALLOC_FAIL;
        }

        msg_st->send = do_work_tx;
        msg_st->cleanup = NULL;

        struct xomp_msg_st *st = (struct xomp_msg_st *) msg_st;
        st->args.do_work.arg = (uint64_t) work->data;
        st->args.do_work.fn = fn;
        st->args.do_work.id = (uint64_t) task;
        st->args.do_work.flags = 0;

        txq_send(msg_st);
    }

    return SYS_ERR_OK;
}
