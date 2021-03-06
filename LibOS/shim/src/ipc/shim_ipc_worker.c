/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2021 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#include <stdalign.h>
#include <stdint.h>
#include <stdnoreturn.h>

#include "assert.h"
#include "cpu.h"
#include "list.h"
#include "pal.h"
#include "shim_handle.h"
#include "shim_internal.h"
#include "shim_ipc.h"
#include "shim_lock.h"
#include "shim_thread.h"
#include "shim_types.h"
#include "shim_utils.h"

#define LOG_PREFIX "IPC worker: "

DEFINE_LIST(shim_ipc_connection);
DEFINE_LISTP(shim_ipc_connection);
struct shim_ipc_connection {
    LIST_TYPE(shim_ipc_connection) list;
    PAL_HANDLE handle;
    IDTYPE vmid;
};

/* List of incoming IPC connections, fully managed by this IPC worker thread (hence no locking
 * needed). */
static LISTP_TYPE(shim_ipc_connection) g_ipc_connections;
static size_t g_ipc_connections_cnt = 0;

static struct shim_thread* g_worker_thread = NULL;
static AEVENTTYPE exit_notification_event;
/* Used by `DkThreadExit` to indicate that the thread really exited and is not using any resources
 * (e.g. stack) anymore. Awaited to be `0` (thread exited) in `terminate_ipc_worker()`. */
static int g_clear_on_worker_exit = 1;
static PAL_HANDLE g_self_ipc_handle = NULL;

static int ipc_resp_callback(struct shim_ipc_msg* msg, IDTYPE src);
static int ipc_connect_back_callback(struct shim_ipc_msg* msg, IDTYPE src);

typedef int (*ipc_callback)(struct shim_ipc_msg* msg, IDTYPE src);
static ipc_callback ipc_callbacks[] = {
    [IPC_MSG_RESP]          = ipc_resp_callback,
    [IPC_MSG_CONNBACK]      = ipc_connect_back_callback,
    [IPC_MSG_DUMMY]         = ipc_dummy_callback,
    [IPC_MSG_CHILDEXIT]     = ipc_cld_exit_callback,
    [IPC_MSG_LEASE]         = ipc_lease_callback,
    [IPC_MSG_OFFER]         = ipc_offer_callback,
    [IPC_MSG_SUBLEASE]      = ipc_sublease_callback,
    [IPC_MSG_QUERY]         = ipc_query_callback,
    [IPC_MSG_QUERYALL]      = ipc_queryall_callback,
    [IPC_MSG_ANSWER]        = ipc_answer_callback,
    [IPC_MSG_PID_KILL]      = ipc_pid_kill_callback,
    [IPC_MSG_PID_GETSTATUS] = ipc_pid_getstatus_callback,
    [IPC_MSG_PID_RETSTATUS] = ipc_pid_retstatus_callback,
    [IPC_MSG_PID_GETMETA]   = ipc_pid_getmeta_callback,
    [IPC_MSG_PID_RETMETA]   = ipc_pid_retmeta_callback,
    [IPC_MSG_SYSV_FINDKEY]  = ipc_sysv_findkey_callback,
    [IPC_MSG_SYSV_TELLKEY]  = ipc_sysv_tellkey_callback,
    [IPC_MSG_SYSV_DELRES]   = ipc_sysv_delres_callback,
    [IPC_MSG_SYSV_MSGSND]   = ipc_sysv_msgsnd_callback,
    [IPC_MSG_SYSV_MSGRCV]   = ipc_sysv_msgrcv_callback,
    [IPC_MSG_SYSV_SEMOP]    = ipc_sysv_semop_callback,
    [IPC_MSG_SYSV_SEMCTL]   = ipc_sysv_semctl_callback,
    [IPC_MSG_SYSV_SEMRET]   = ipc_sysv_semret_callback,
};

static void ipc_leader_died_callback(void) {
    /* This might happen legitimately e.g. if IPC leader is also our parent and does `wait` + `exit`
     * If this is an erroneous disconnect it will be noticed when trying to communicate with
     * the leader. */
    log_debug("IPC leader disconnected\n");
}

static void disconnect_callbacks(struct shim_ipc_connection* conn) {
    if (g_process_ipc_ids.leader_vmid == conn->vmid) {
        ipc_leader_died_callback();
    }
    ipc_child_disconnect_callback(conn->vmid);

    /*
     * Currently outgoing IPC connections (handled in `shim_ipc.c`) are not cleaned up - there is
     * no place that can notice disconnection of an outgoing connection other than a failure to send
     * data via such connection. We try to remove an outgoing IPC connection to a process that just
     * disconnected here - usually we have connections set up in both ways.
    */
    remove_outgoing_ipc_connection(conn->vmid);
}

static int add_ipc_connection(PAL_HANDLE handle, IDTYPE id) {
    struct shim_ipc_connection* conn = malloc(sizeof(*conn));
    if (!conn) {
        return -ENOMEM;
    }

    conn->handle = handle;
    conn->vmid = id;

    LISTP_ADD(conn, &g_ipc_connections, list);
    g_ipc_connections_cnt++;
    return 0;
}

static void del_ipc_connection(struct shim_ipc_connection* conn) {
    LISTP_DEL(conn, &g_ipc_connections, list);
    g_ipc_connections_cnt--;

    DkObjectClose(conn->handle);

    free(conn);
}

static int send_ipc_response(IDTYPE dest, int ret, unsigned long seq) {
    ret = (ret == RESPONSE_CALLBACK) ? 0 : ret;

    size_t total_msg_size = get_ipc_msg_size(sizeof(struct shim_ipc_resp));
    struct shim_ipc_msg* resp_msg = __alloca(total_msg_size);
    init_ipc_msg(resp_msg, IPC_MSG_RESP, total_msg_size, dest);
    resp_msg->seq = seq;

    struct shim_ipc_resp* resp = (struct shim_ipc_resp*)resp_msg->msg;
    resp->retval = ret;

    return send_ipc_message(resp_msg, dest);
}

static void set_request_retval(struct shim_ipc_msg_with_ack* req_msg, void* data) {
    if (!req_msg) {
        log_error(LOG_PREFIX "got response to an unknown message\n");
        return;
    }

    req_msg->retval = (int)(intptr_t)data;
    thread_wakeup(req_msg->thread);
}

static int ipc_resp_callback(struct shim_ipc_msg* msg, IDTYPE src) {
    struct shim_ipc_resp* resp = (struct shim_ipc_resp*)&msg->msg;
    log_debug(LOG_PREFIX "got IPC msg response from %u: %d\n", msg->src, resp->retval);
    assert(src == msg->src);

    ipc_msg_response_handle(src, msg->seq, set_request_retval, (void*)(intptr_t)resp->retval);

    return 0;
}

/* Another process requested that we connect to it. */
static int ipc_connect_back_callback(struct shim_ipc_msg* orig_msg, IDTYPE src) {
    size_t total_msg_size = get_ipc_msg_size(0);
    struct shim_ipc_msg* msg = __alloca(total_msg_size);
    init_ipc_msg(msg, IPC_MSG_DUMMY, total_msg_size, src);
    msg->seq = orig_msg->seq;

    return send_ipc_message(msg, src);
}

/*
 * Receive and handle some (possibly many) messages from IPC connection `conn`.
 * Returns `0` on success, `1` on EOF (connection closed on a message boundary), negative error
 * code on failures.
 */
static int receive_ipc_messages(struct shim_ipc_connection* conn) {
    size_t size = 0;
    /* Try to get more bytes that strictly required in case there are more messages waiting.
     * `0x20` as a random estimation of "couple of ints" + `IPC_MSG_MINIMAL_SIZE` to get the next
     * message header if possible. */
#define READAHEAD_SIZE (0x20 + IPC_MSG_MINIMAL_SIZE)
    union {
        struct shim_ipc_msg msg_header;
        char buf[IPC_MSG_MINIMAL_SIZE + READAHEAD_SIZE];
    } buf;
#undef READAHEAD_SIZE

    do {
        /* Receive at least the message header. */
        while (size < IPC_MSG_MINIMAL_SIZE) {
            size_t tmp_size = sizeof(buf) - size;
            int ret = DkStreamRead(conn->handle, /*offset=*/0, &tmp_size, buf.buf + size, NULL, 0);
            if (ret < 0) {
                if (ret == -PAL_ERROR_INTERRUPTED || ret == -PAL_ERROR_TRYAGAIN) {
                    continue;
                }
                ret = pal_to_unix_errno(ret);
                log_error(LOG_PREFIX "receiving message header from %u failed: %d\n", conn->vmid,
                          ret);
                return ret;
            }
            if (tmp_size == 0) {
                if (size == 0) {
                    /* EOF on the handle, but exactly on the message boundary. */
                    return 1;
                }
                log_error(LOG_PREFIX "receiving message from %u failed: remote closed early\n",
                          conn->vmid);
                return -ENODATA;
            }
            size += tmp_size;
        }

        size_t msg_size = buf.msg_header.size;
        struct shim_ipc_msg* msg = malloc(msg_size);
        if (!msg) {
            return -ENOMEM;
        }

        if (msg_size <= size) {
            /* Already got the whole message (and possibly part of the next one). */
            memcpy(msg, buf.buf, msg_size);
            memmove(buf.buf, buf.buf + msg_size, size - msg_size);
            size -= msg_size;
        } else {
            /* Need to get rest of the message. */
            memcpy(msg, buf.buf, size);
            int ret = read_exact(conn->handle, (char*)msg + size, msg_size - size);
            if (ret < 0) {
                free(msg);
                log_error(LOG_PREFIX "receiving message from %u failed: %d\n", conn->vmid, ret);
                return ret;
            }
            size = 0;
        }

        log_debug(LOG_PREFIX "received IPC message from %u: code=%d size=%lu src=%u dst=%u seq=%lu"
                  "\n", conn->vmid, msg->code, msg->size, msg->src, msg->dst, msg->seq);

        assert(conn->vmid == msg->src);

        if (msg->code < ARRAY_SIZE(ipc_callbacks) && ipc_callbacks[msg->code]) {
            int ret = ipc_callbacks[msg->code](msg, conn->vmid);
            if ((ret < 0 || ret == RESPONSE_CALLBACK) && msg->seq) {
                ret = send_ipc_response(conn->vmid, ret, msg->seq);
                if (ret < 0) {
                    log_error(LOG_PREFIX "sending IPC msg response to %u failed: %d\n", conn->vmid,
                              ret);
                    free(msg);
                    return ret;
                }
            }
        } else {
            log_error(LOG_PREFIX "received unknown IPC msg type: %u\n", msg->code);
        }

        free(msg);
    } while (size > 0);

    return 0;
}

static noreturn void ipc_worker_main(void) {
    /* TODO: If we had a global array of connections (instead of a list) we wouldn't have to gather
     * them all here in every loop iteration, but then deletion would be slower (but deletion should
     * be rare). */
    struct shim_ipc_connection** connections = NULL;
    PAL_HANDLE* handles = NULL;
    PAL_FLG* events = NULL;
    PAL_FLG* ret_events = NULL;
    size_t prev_items_cnt = 0;

    while (1) {
        /* Reserve 2 slots for `exit_notification_event` and `g_self_ipc_handle`. */
        const size_t reserved_slots = 2;
        size_t items_cnt = g_ipc_connections_cnt + reserved_slots;
        if (items_cnt != prev_items_cnt) {
            free(connections);
            free(handles);
            free(events);
            free(ret_events);

            connections = malloc(items_cnt * sizeof(*connections));
            handles = malloc(items_cnt * sizeof(*handles));
            events = malloc(items_cnt * sizeof(*events));
            ret_events = malloc(items_cnt * sizeof(*ret_events));
            if (!connections || !handles || !events || !ret_events) {
                log_error(LOG_PREFIX "arrays allocation failed\n");
                goto out_die;
            }

            prev_items_cnt = items_cnt;
        }

        memset(ret_events, 0, items_cnt * sizeof(*ret_events));

        connections[0] = NULL;
        handles[0] = event_handle(&exit_notification_event);
        events[0] = PAL_WAIT_READ;
        connections[1] = NULL;
        handles[1] = g_self_ipc_handle;
        events[1] = PAL_WAIT_READ;

        struct shim_ipc_connection* conn;
        size_t i = reserved_slots;
        LISTP_FOR_EACH_ENTRY(conn, &g_ipc_connections, list) {
            connections[i] = conn;
            handles[i] = conn->handle;
            events[i] = PAL_WAIT_READ;
            /* `ret_events[i]` already cleared. */
            i++;
        }

        int ret = DkStreamsWaitEvents(items_cnt, handles, events, ret_events, NO_TIMEOUT);
        if (ret < 0) {
            if (ret == -PAL_ERROR_INTERRUPTED) {
                /* Generally speaking IPC worker should not be interrupted, but this happens with
                 * SGX exitless feature. */
                continue;
            }
            ret = pal_to_unix_errno(ret);
            log_error(LOG_PREFIX "DkStreamsWaitEvents failed: %d\n", ret);
            goto out_die;
        }

        if (ret_events[0]) {
            /* `exit_notification_event`. */
            if (ret_events[0] & ~PAL_WAIT_READ) {
                log_error(LOG_PREFIX "unexpected event (%d) on exit handle\n", ret_events[0]);
                goto out_die;
            }
            log_debug(LOG_PREFIX "exiting worker thread\n");

            free(connections);
            free(handles);
            free(events);
            free(ret_events);

            struct shim_thread* cur_thread = get_cur_thread();
            assert(g_worker_thread == cur_thread);
            assert(cur_thread->shim_tcb->tp == cur_thread);
            cur_thread->shim_tcb->tp = NULL;
            put_thread(cur_thread);

            DkThreadExit(&g_clear_on_worker_exit);
            /* Unreachable. */
        }

        if (ret_events[1]) {
            /* New connection incoming. */
            if (ret_events[1] & ~PAL_WAIT_READ) {
                log_error(LOG_PREFIX "unexpected event (%d) on listening handle\n", ret_events[1]);
                goto out_die;
            }
            PAL_HANDLE new_handle = NULL;
            ret = DkStreamWaitForClient(g_self_ipc_handle, &new_handle);
            if (ret < 0) {
                ret = pal_to_unix_errno(ret);
                log_error(LOG_PREFIX "DkStreamWaitForClient failed: %d\n", ret);
                goto out_die;
            }
            IDTYPE new_id = 0;
            ret = read_exact(new_handle, &new_id, sizeof(new_id));
            if (ret < 0) {
                log_error(LOG_PREFIX "receiving id failed: %d\n", ret);
                DkObjectClose(new_handle);
            } else {
                ret = add_ipc_connection(new_handle, new_id);
                if (ret < 0) {
                    log_error(LOG_PREFIX "add_ipc_connection failed: %d\n", ret);
                    goto out_die;
                }
            }
        }

        for (i = reserved_slots; i < items_cnt; i++) {
            conn = connections[i];
            if (ret_events[i] & PAL_WAIT_READ) {
                ret = receive_ipc_messages(conn);
                if (ret == 1) {
                    /* Connection closed. */
                    disconnect_callbacks(conn);
                    del_ipc_connection(conn);
                    continue;
                }
                if (ret < 0) {
                    log_error(LOG_PREFIX "failed to receive an IPC message from %u: %d\n",
                              conn->vmid, ret);
                    /* Let the code below handle this error. */
                    ret_events[i] = PAL_WAIT_ERROR;
                }
            }
            /* If there was something else other than error reported, let the loop spin at least one
             * more time - in case there are messages left to be read. */
            if (ret_events[i] == PAL_WAIT_ERROR) {
                disconnect_callbacks(conn);
                del_ipc_connection(conn);
            }
        }
    }

out_die:
    DkProcessExit(1);
}

static void ipc_worker_wrapper(void* arg) {
    __UNUSED(arg);
    assert(g_worker_thread);

    shim_tcb_init();
    set_cur_thread(g_worker_thread);

    log_setprefix(shim_get_tcb());

    log_debug("IPC worker started\n");
    ipc_worker_main();
    /* Unreachable. */
}

static int init_self_ipc_handle(void) {
    char uri[PIPE_URI_SIZE];
    return create_pipe(NULL, uri, sizeof(uri), &g_self_ipc_handle, NULL,
                       /*use_vmid_for_name=*/true);
}

static int create_ipc_worker(void) {
    int ret = init_self_ipc_handle();
    if (ret < 0) {
        return ret;
    }

    g_worker_thread = get_new_internal_thread();
    if (!g_worker_thread) {
        return -ENOMEM;
    }

    PAL_HANDLE handle = NULL;
    ret = DkThreadCreate(ipc_worker_wrapper, NULL, &handle);
    if (ret < 0) {
        put_thread(g_worker_thread);
        g_worker_thread = NULL;
        return pal_to_unix_errno(ret);
    }

    g_worker_thread->pal_handle = handle;

    return 0;
}

int init_ipc_worker(void) {
    int ret = create_event(&exit_notification_event);
    if (ret < 0) {
        return ret;
    }

    enable_locking();
    return create_ipc_worker();
}

void terminate_ipc_worker(void) {
    set_event(&exit_notification_event, 1);

    while (__atomic_load_n(&g_clear_on_worker_exit, __ATOMIC_RELAXED)) {
        CPU_RELAX();
    }

    put_thread(g_worker_thread);
    g_worker_thread = NULL;
    DkObjectClose(g_self_ipc_handle);
    g_self_ipc_handle = NULL;
}
