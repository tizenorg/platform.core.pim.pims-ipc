/*
 * PIMS IPC
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the License);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pims-internal.h>
#include <pims-debug.h>
#include <pims-socket.h>
#include <pims-ipc-data.h>
#include <pims-ipc-svc.h>

#define PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT  2

typedef struct
{
    char *service;
    gid_t group;
    mode_t mode;
    GHashTable *cb_table;
    GHashTable *client_table;
    GList *workers;
    GList *requests;
    int workers_max_count;
    void* context;
    void* router;
    void* worker;
    void* manager;
    void* monitor;
} pims_ipc_svc_t;

typedef struct
{
    char *service;
    gid_t group;
    mode_t mode;
    void* context;
    void* publisher;
} pims_ipc_svc_for_publish_t;


typedef struct
{
    pims_ipc_svc_call_cb callback;
    void * user_data;
} pims_ipc_svc_cb_t;

static pims_ipc_svc_t *_g_singleton = NULL;
static pims_ipc_svc_for_publish_t *_g_singleton_for_publish = NULL;

API int pims_ipc_svc_init(char *service, gid_t group, mode_t mode)
{
    if (_g_singleton)
    {
        ERROR("Already exist");
        return -1;
    }

    _g_singleton = g_new0(pims_ipc_svc_t, 1);
    _g_singleton->service = g_strdup(service);
    _g_singleton->group = group;
    _g_singleton->mode = mode;
    _g_singleton->workers_max_count = PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT;
    _g_singleton->cb_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ASSERT(_g_singleton->cb_table);
    _g_singleton->client_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ASSERT(_g_singleton->client_table);

    return 0;
}

static void __free_zmq_msg(gpointer data)
{
    zmq_msg_t *lpzmsg = data;

    if (lpzmsg)
    {
        zmq_msg_close(lpzmsg);
        g_free(lpzmsg);
    }
}

API int pims_ipc_svc_deinit(void)
{
    if (!_g_singleton)
        return -1;

    g_free(_g_singleton->service);
    g_hash_table_destroy(_g_singleton->cb_table);
    g_hash_table_destroy(_g_singleton->client_table);
    g_list_free(_g_singleton->workers);
    g_list_free_full(_g_singleton->requests, __free_zmq_msg);
    g_free(_g_singleton);
    _g_singleton = NULL;

    return 0;
}

API int pims_ipc_svc_register(char *module, char *function, pims_ipc_svc_call_cb callback, void *userdata)
{
    pims_ipc_svc_cb_t *cb_data = NULL;
    gchar *call_id = NULL;

    if (!module || !function || !callback)
    {
        ERROR("Invalid argument");
        return -1;
    }
    cb_data = g_new0(pims_ipc_svc_cb_t, 1);
    call_id = PIMS_IPC_MAKE_CALL_ID(module, function);

    VERBOSE("register cb id[%s]", call_id);
    cb_data->callback = callback;
    cb_data->user_data = userdata;
    g_hash_table_insert(_g_singleton->cb_table, call_id, cb_data);

    return 0;
}

API int pims_ipc_svc_init_for_publish(char *service, gid_t group, mode_t mode)
{
    if (_g_singleton_for_publish)
    {
        ERROR("Already exist");
        return -1;
    }

    _g_singleton_for_publish = g_new0(pims_ipc_svc_for_publish_t, 1);
    _g_singleton_for_publish->service = g_strdup(service);
    _g_singleton_for_publish->group = group;
    _g_singleton_for_publish->mode = mode;

    return 0;
}

API int pims_ipc_svc_deinit_for_publish(void)
{
    if (!_g_singleton_for_publish)
        return -1;

    g_free(_g_singleton_for_publish->service);
    g_free(_g_singleton_for_publish);
    _g_singleton_for_publish = NULL;

    return 0;
}

static void __pims_ipc_svc_data_free_cb(void *data, void *hint)
{
    if (hint)
        g_free(hint);
}

API int pims_ipc_svc_publish(char *module, char *event, pims_ipc_data_h data)
{
    pims_ipc_svc_for_publish_t *ipc_svc = _g_singleton_for_publish;
    gboolean is_valid = FALSE;
    gchar *call_id = PIMS_IPC_MAKE_CALL_ID(module, event);

    // init messages
    zmq_msg_t call_id_msg;
    zmq_msg_t data_msg;

    zmq_msg_init_data(&call_id_msg, call_id, strlen(call_id) + 1, __pims_ipc_svc_data_free_cb, call_id);
    VERBOSE("call id = %s", (char*)zmq_msg_data(&call_id_msg));

    zmq_msg_init(&data_msg);

    do {
        if (data == NULL)
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, ipc_svc->publisher, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }
        else
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, ipc_svc->publisher, ZMQ_SNDMORE) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }

            // marshal data
            if (pims_ipc_data_marshal_with_zmq(data, &data_msg) != 0)
            {
                ERROR("marshal error");
                break;
            }

            VERBOSE("the size of sending data = %d", zmq_msg_size(&data_msg));

            // send data
            if (_pims_zmq_msg_send(&data_msg, ipc_svc->publisher, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }

        is_valid = TRUE;
    } while (0);

    zmq_msg_close(&call_id_msg);
    zmq_msg_close(&data_msg);

    if (is_valid == FALSE)
        return -1;
    return 0;
}

static void __run_callback(int worker_id, char *call_id, pims_ipc_data_h dhandle_in, pims_ipc_data_h *dhandle_out)
{
    pims_ipc_svc_cb_t *cb_data = NULL;

    VERBOSE("Call id [%s]", call_id);

    cb_data = (pims_ipc_svc_cb_t*)g_hash_table_lookup(_g_singleton->cb_table, call_id);
    if (cb_data == NULL)
    {
        VERBOSE("unable to find %s", call_id);
        return;
    }
    
    cb_data->callback((pims_ipc_h)worker_id, dhandle_in, dhandle_out, cb_data->user_data);
}

static int __process_worker_task(int worker_id, void *context, void *worker)
{
    gboolean is_create = FALSE;
    gboolean is_destroy = FALSE;
    char *pid = NULL;
    char *call_id = NULL;
    int64_t more = 0;
    size_t more_size = sizeof(more);
    pims_ipc_data_h dhandle_in = NULL;
    pims_ipc_data_h dhandle_out = NULL;

    VERBOSE("");

#ifdef _TEST
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("worker time[%lu:%lu]\n", tv.tv_sec, tv.tv_usec);
#endif

    zmq_msg_t pid_msg;
    zmq_msg_t sequence_no_msg;
    zmq_msg_t call_id_msg;
    zmq_msg_t data_msg;
    
    zmq_msg_init(&pid_msg);
    zmq_msg_init(&sequence_no_msg);
    zmq_msg_init(&call_id_msg);
    zmq_msg_init(&data_msg);

    do {
        // read pid
        if (_pims_zmq_msg_recv(&pid_msg, worker, 0) == -1)
        {
            ERROR("recv error : %s", zmq_strerror(errno));
            break;
        }

        // read sequence no
        if (_pims_zmq_msg_recv(&sequence_no_msg, worker, 0) == -1)
        {
            ERROR("recv error : %s", zmq_strerror(errno));
            break;
        }

        // read call id
        if (_pims_zmq_msg_recv(&call_id_msg, worker, 0) == -1)
        {
            ERROR("recv error : %s", zmq_strerror(errno));
            break;
        }

        more = 0;
        zmq_getsockopt(worker, ZMQ_RCVMORE, &more, &more_size);
        if (more)
        {
            // read data
            if (_pims_zmq_msg_recv(&data_msg, worker, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }

            dhandle_in = pims_ipc_data_unmarshal_with_zmq(&data_msg);
            if (dhandle_in == NULL)
            {
                ERROR("unmarshal error");
                break;
            }
        }

        pid = (char*)zmq_msg_data(&pid_msg);
        ASSERT(pid);
        VERBOSE("client pid = %s", pid);

        call_id = (char*)zmq_msg_data(&call_id_msg);
        ASSERT(call_id);
        VERBOSE("call_id = [%s]", call_id);

        // call a callback function with call id and data
        if (strcmp(PIMS_IPC_CALL_ID_CREATE, call_id) == 0)
        {
            is_create = TRUE;
        }
        else if (strcmp(PIMS_IPC_CALL_ID_DESTROY, call_id) == 0)
        {
            is_destroy = TRUE;
        }
        else
        {
            __run_callback(worker_id, call_id, dhandle_in, &dhandle_out);
        }

        // send pid
        if (_pims_zmq_msg_send(&pid_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        // send empty 
        zmq_msg_t empty_msg;
        zmq_msg_init_size(&empty_msg, 0);
        if (_pims_zmq_msg_send(&empty_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            zmq_msg_close(&empty_msg);
            break;
        }
        zmq_msg_close(&empty_msg);

        // send sequence no
        if (_pims_zmq_msg_send(&sequence_no_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        if (dhandle_out)
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, worker, ZMQ_SNDMORE) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }

            // marshal data
            zmq_msg_close(&data_msg);
            zmq_msg_init(&data_msg);
            if (pims_ipc_data_marshal_with_zmq(dhandle_out, &data_msg) != 0)
            {
                ERROR("marshal error");
                break;
            }

            // send data
            VERBOSE("the size of sending data = %d", zmq_msg_size(&data_msg));
            if (_pims_zmq_msg_send(&data_msg, worker, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }
        else
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, worker, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }
    } while (0);

    zmq_msg_close(&pid_msg);
    zmq_msg_close(&sequence_no_msg);
    zmq_msg_close(&call_id_msg);
    zmq_msg_close(&data_msg);

    if (dhandle_in)
        pims_ipc_data_destroy(dhandle_in);
    if (dhandle_out)
        pims_ipc_data_destroy(dhandle_out);

    VERBOSE("responsed");

#ifdef _TEST
    gettimeofday(&tv, NULL);
    printf("worker time[%lu:%lu]\n", tv.tv_sec, tv.tv_usec);
#endif

    if (is_destroy)
        return -1;
    return 0;
}

static int __process_manager_task(int worker_id, void *context, void *manager)
{
    VERBOSE("");

    // read pid
    zmq_msg_t pid_msg;
    zmq_msg_init(&pid_msg);
    if (_pims_zmq_msg_recv(&pid_msg, manager, 0) == -1)
    {
        ERROR("recv error : %s", zmq_strerror(errno));
        zmq_msg_close(&pid_msg);
        return -1;
    }
    zmq_msg_close(&pid_msg);

    return -1;
}

static void* __worker_loop(void *args)
{
    void *context = args;
    int worker_id = (int)pthread_self();
    char *path = NULL;

    void *worker = zmq_socket(context, ZMQ_DEALER);
    if (!worker)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        return NULL;
    }
    if (zmq_setsockopt(worker, ZMQ_IDENTITY, &worker_id, sizeof(int)) != 0)
    {
        ERROR("setsockopt error : %s", zmq_strerror(errno));
        zmq_close(worker);
        return NULL;
    }
    path = g_strdup_printf("inproc://%s-%s", _g_singleton->service, PIMS_IPC_DEALER_PATH);
    if (zmq_connect(worker, path) != 0)
    {
        ERROR("connect error : %s", zmq_strerror(errno));
        g_free(path);
        zmq_close(worker);
        return NULL;
    }
    g_free(path);

    // send the ID of a worker to the manager
    void *manager = zmq_socket(context, ZMQ_DEALER);
    if (!manager)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        zmq_close(worker);
        return NULL;
    }
    if (zmq_setsockopt(manager, ZMQ_IDENTITY, &worker_id, sizeof(int)) != 0)
    {
        ERROR("setsockopt error : %s", zmq_strerror(errno));
        zmq_close(manager);
        zmq_close(worker);
        return NULL;
    }
    path = g_strdup_printf("inproc://%s-%s", _g_singleton->service, PIMS_IPC_MANAGER_PATH);
    if (zmq_connect(manager, path) != 0)
    {
        ERROR("connect error : %s", zmq_strerror(errno));
        g_free(path);
        zmq_close(manager);
        zmq_close(worker);
        return NULL;
    }
    g_free(path);

    VERBOSE("starting worker id: %x", worker_id);
    zmq_msg_t message;
    zmq_msg_init_size(&message, sizeof(int));
    memcpy(zmq_msg_data(&message), &worker_id, sizeof(int));
    if (_pims_zmq_msg_send(&message, manager, 0) == -1)
    {
        ERROR("send error : %s", zmq_strerror(errno));
        zmq_msg_close(&message);
        zmq_close(manager);
        zmq_close(worker);
        return NULL;
    }
    zmq_msg_close(&message);

    // poll all sockets
    while (1)
    {
        zmq_pollitem_t items[] = {
            {worker, 0, ZMQ_POLLIN, 0},
            {manager, 0, ZMQ_POLLIN, 0}
        };

        if (zmq_poll(items, 2, -1) == -1)
        {
            ERROR("poll error : %s", zmq_strerror(errno));
            break;
        }

        if (items[0].revents & ZMQ_POLLIN)
        {
            if (__process_worker_task(worker_id, context, worker) != 0)
                break;
        }

        if (items[1].revents & ZMQ_POLLIN)
        {
            if (__process_manager_task(worker_id, context, manager) != 0)
                break;
        }
    }

    VERBOSE("terminating worker id: %x", worker_id);

    zmq_close(manager);
    zmq_close(worker);
    return NULL;
}

static void __launch_worker(void *(*start_routine) (void *), void *context)
{
    pthread_t worker;
    pthread_attr_t attr;

    // set kernel thread
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

    pthread_create(&worker, &attr, start_routine, context);
    // detach this thread
    pthread_detach(worker);
}

static gboolean __is_worker_available()
{
    if (_g_singleton->workers)
        return TRUE;
    else
        return FALSE;
}

static int __get_worker(const char *pid, int *worker_id)
{
    ASSERT(pid);
    ASSERT(worker_id);

    if (!__is_worker_available())
    {
        ERROR("There is no idle worker");
        return -1;
    }
    *worker_id = (int)(g_list_first(_g_singleton->workers)->data);
    _g_singleton->workers = g_list_delete_link(_g_singleton->workers,
            g_list_first(_g_singleton->workers));

    g_hash_table_insert(_g_singleton->client_table, g_strdup(pid), GINT_TO_POINTER(*worker_id));

    return 0;
}

static int __find_worker(const char *pid, int *worker_id)
{
    char *orig_pid = NULL;

    ASSERT(pid);
    ASSERT(worker_id);
    
    if (g_hash_table_lookup_extended(_g_singleton->client_table, pid,
                (gpointer*)&orig_pid, (gpointer*)worker_id) == TRUE)
    {
        VERBOSE("found worker id for %s = %x", pid, *worker_id);
        return 0;
    }
    else
    {
        VERBOSE("unable to find worker id for %s", pid);
        return -1;
    }
}

static void __remove_worker(const char *pid)
{
    g_hash_table_remove(_g_singleton->client_table, pid);
}

static void __terminate_worker(void *manager, int worker_id, const char *pid)
{
    // send worker id
    zmq_msg_t worker_id_msg;
    zmq_msg_init_size(&worker_id_msg, sizeof(int));
    memcpy(zmq_msg_data(&worker_id_msg), &worker_id, sizeof(int));
    if (_pims_zmq_msg_send(&worker_id_msg, manager, ZMQ_SNDMORE) == -1)
    {
        ERROR("send error : %s", zmq_strerror(errno));
        zmq_msg_close(&worker_id_msg);
        return;
    }
    zmq_msg_close(&worker_id_msg);

    // send pid
    zmq_msg_t pid_msg;
    zmq_msg_init_data(&pid_msg, (char*)pid, strlen(pid) + 1, NULL, NULL);
    if (_pims_zmq_msg_send(&pid_msg, manager, 0) == -1)
    {
        ERROR("send error : %s", zmq_strerror(errno));
        zmq_msg_close(&pid_msg);
        return;
    }
    zmq_msg_close(&pid_msg);
}

static gboolean __enqueue_zmq_msg(zmq_msg_t *zmsg)
{
    zmq_msg_t *lpzmsg = NULL;
    
    if (zmsg)
    {
        lpzmsg = g_new0(zmq_msg_t, 1);
        zmq_msg_init(lpzmsg);
        zmq_msg_copy(lpzmsg, zmsg);
    }
    _g_singleton->requests = g_list_append(_g_singleton->requests, lpzmsg);

    return TRUE;
}

static gboolean __dequeue_zmq_msg(zmq_msg_t *zmsg)
{
    zmq_msg_t *lpzmsg = NULL;

    ASSERT(_g_singleton->requests);
    lpzmsg = (zmq_msg_t*)(g_list_first(_g_singleton->requests)->data);
    _g_singleton->requests = g_list_delete_link(_g_singleton->requests,
            g_list_first(_g_singleton->requests));

    if (lpzmsg == NULL)
        return FALSE;

    zmq_msg_copy(zmsg, lpzmsg);
    zmq_msg_close(lpzmsg);
    g_free(lpzmsg);

    return TRUE;
}

static int __process_router_event(void *context, void *router, void *worker, gboolean for_queue)
{
    char *pid = NULL;
    char *call_id = NULL;
    int64_t more = 0;
    size_t more_size = sizeof(more);
    int worker_id = -1;
    gboolean is_with_data = FALSE;
    gboolean is_valid = FALSE;

#ifdef _TEST
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("router time[%lu:%lu]\n", tv.tv_sec, tv.tv_usec);
#endif

    // init messages for receiving
    zmq_msg_t pid_msg;
    zmq_msg_t sequence_no_msg;
    zmq_msg_t call_id_msg;
    zmq_msg_t data_msg;
    
    zmq_msg_init(&pid_msg);
    zmq_msg_init(&sequence_no_msg);
    zmq_msg_init(&call_id_msg);
    zmq_msg_init(&data_msg);

    // relay a request from a client to a worker
    do {
        if (for_queue)
        {
            // dequeue a request
            __dequeue_zmq_msg(&pid_msg);
            __dequeue_zmq_msg(&sequence_no_msg);
            __dequeue_zmq_msg(&call_id_msg);
            is_with_data = __dequeue_zmq_msg(&data_msg);

            if (is_with_data)
                __dequeue_zmq_msg(NULL);
        }
        else
        {
            // read pid
            if (_pims_zmq_msg_recv(&pid_msg, router, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }

            // read empty and kill
            zmq_msg_t empty_msg;
            zmq_msg_init(&empty_msg);
            if (_pims_zmq_msg_recv(&empty_msg, router, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                zmq_msg_close(&empty_msg);
                break;
            }
            zmq_msg_close(&empty_msg);

            // read sequence no
            more = 0;
            zmq_getsockopt(router, ZMQ_RCVMORE, &more, &more_size);
            if (!more)
            {
                ERROR("recv error : corrupted message");
                break;
            }
            if (_pims_zmq_msg_recv(&sequence_no_msg, router, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }

            // read call id
            more = 0;
            zmq_getsockopt(router, ZMQ_RCVMORE, &more, &more_size);
            if (!more)
            {
                ERROR("recv error : corrupted message");
                break;
            }
            if (_pims_zmq_msg_recv(&call_id_msg, router, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }

            // read data
            more = 0;
            zmq_getsockopt(router, ZMQ_RCVMORE, &more, &more_size);
            if (more)
            {
                is_with_data = TRUE;
                if (_pims_zmq_msg_recv(&data_msg, router, 0) == -1)
                {
                    ERROR("recv error : %s", zmq_strerror(errno));
                    break;
                }
            }
        }

        pid = zmq_msg_data(&pid_msg);
        ASSERT(pid != NULL);
        VERBOSE("client pid = %s", pid);

        call_id = (char*)zmq_msg_data(&call_id_msg);
        ASSERT(call_id != NULL);
        VERBOSE("call_id = [%s], create_call_id = [%s]", PIMS_IPC_CALL_ID_CREATE, call_id);
        if (strcmp(PIMS_IPC_CALL_ID_CREATE, call_id) == 0)
        {
            // Get a worker. If cannot get a worker, create a worker and enqueue a current request 
            __launch_worker(__worker_loop, context);
            if (__get_worker((const char*)pid, &worker_id) != 0)
            {
                // enqueue a request until a new worker will be registered
                __enqueue_zmq_msg(&pid_msg);
                __enqueue_zmq_msg(&sequence_no_msg);
                __enqueue_zmq_msg(&call_id_msg);
                if (is_with_data)
                    __enqueue_zmq_msg(&data_msg);
                __enqueue_zmq_msg(NULL);

                is_valid = TRUE;
                break;
            }
        }
        else
        {
            // Find a worker
            if (__find_worker((const char*)pid, &worker_id) != 0)
            {
                ERROR("unable to find a worker");
                break;
            }

            if (strcmp(PIMS_IPC_CALL_ID_DESTROY, call_id) == 0)
            {
                __remove_worker((const char*)pid);
            }
        }

        VERBOSE("routing worker id = %x", worker_id);
        // send worker id
        zmq_msg_t worker_id_msg;
        zmq_msg_init_size(&worker_id_msg, sizeof(int));
        memcpy(zmq_msg_data(&worker_id_msg), &worker_id, sizeof(int));
        if (_pims_zmq_msg_send(&worker_id_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            zmq_msg_close(&worker_id_msg);
            break;
        }
        zmq_msg_close(&worker_id_msg);

        // send pid
        if (_pims_zmq_msg_send(&pid_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        // send sequence no
        if (_pims_zmq_msg_send(&sequence_no_msg, worker, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        // send call id
        if (_pims_zmq_msg_send(&call_id_msg, worker, is_with_data?ZMQ_SNDMORE:0) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        // send data
        if (is_with_data)
        {
            if (_pims_zmq_msg_send(&data_msg, worker, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }

        is_valid = TRUE;
    } while (0);

    zmq_msg_close(&pid_msg);
    zmq_msg_close(&sequence_no_msg);
    zmq_msg_close(&call_id_msg);
    zmq_msg_close(&data_msg);

#ifdef _TEST
    gettimeofday(&tv, NULL);
    printf("router time[%lu:%lu]\n", tv.tv_sec, tv.tv_usec);
#endif

    if (is_valid == FALSE)
        return -1;
    
    return 0;
}

static int __process_worker_event(void *context, void *worker, void *router)
{
    zmq_msg_t message;
    int64_t more = 0;
    size_t more_size = sizeof(more);

    // Remove worker_id
    zmq_msg_init(&message);
    if (_pims_zmq_msg_recv(&message, worker, 0) == -1)
    {
        ERROR("recv error : %s", zmq_strerror(errno));
    }
    zmq_msg_close(&message);

    while (1)
    {
        //  Process all parts of the message
        zmq_msg_init(&message);
        if (_pims_zmq_msg_recv(&message, worker, 0) == -1)
        {
            ERROR("recv error : %s", zmq_strerror(errno));
        }
        more = 0;
        zmq_getsockopt(worker, ZMQ_RCVMORE, &more, &more_size);
        VERBOSE("router received a message : more[%u]", (unsigned int)more);
        if (_pims_zmq_msg_send(&message, router, more?ZMQ_SNDMORE:0) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
        }
        zmq_msg_close(&message);
        if (!more)
            break;      //  Last message part
    }

    return 0;
}

static int __process_manager_event(void *context, void *manager)
{
    zmq_msg_t worker_id_msg;
    int worker_id = -1;
    
    zmq_msg_init(&worker_id_msg);
    if (_pims_zmq_msg_recv(&worker_id_msg, manager, 0) == -1)
    {
        ERROR("recv error : %s", zmq_strerror(errno));
        zmq_msg_close(&worker_id_msg);
        return -1;
    }
    zmq_msg_close(&worker_id_msg);

    zmq_msg_init(&worker_id_msg);
    if (_pims_zmq_msg_recv(&worker_id_msg, manager, 0) == -1)
    {
        ERROR("recv error : %s", zmq_strerror(errno));
        zmq_msg_close(&worker_id_msg);
        return -1;
    }
    memcpy(&worker_id, zmq_msg_data(&worker_id_msg), sizeof(int));
    zmq_msg_close(&worker_id_msg);

    VERBOSE("registered worker id = %x", worker_id);
    _g_singleton->workers = g_list_append(_g_singleton->workers, GINT_TO_POINTER(worker_id));

    return 0;
}

static int __process_monitor_event(void *context, void *monitor, void *manager)
{
    int worker_id = -1;
    char *pid = NULL;
    zmq_msg_t pid_msg;

    VERBOSE("");
    
    // read pid
    zmq_msg_init(&pid_msg);
    if (_pims_zmq_msg_recv(&pid_msg, monitor, 0) == -1)
    {
        ERROR("recv error : %s", zmq_strerror(errno));
        zmq_msg_close(&pid_msg);
        return -1;
    }

    pid = (char*)zmq_msg_data(&pid_msg);
    ASSERT(pid);
    VERBOSE("client pid = %s", pid);

    if (__find_worker(pid, &worker_id) != 0)
        return 0;

    VERBOSE("found worker id for %s = %x", pid, worker_id);
    
    __terminate_worker(manager, worker_id, pid);
    __remove_worker(pid);

    zmq_msg_close(&pid_msg);

    return 0;
}

static void __client_closed_cb(const char *pid, void *data)
{
	pims_ipc_svc_t *ipc_svc = (pims_ipc_svc_t*)data;
    
    VERBOSE("client pid = %s", pid);

    zmq_msg_t pid_msg;
    zmq_msg_init_size(&pid_msg, strlen(pid) + 1);
    memcpy(zmq_msg_data(&pid_msg), pid, strlen(pid) + 1);
    if (_pims_zmq_msg_send(&pid_msg, ipc_svc->monitor, 0) == -1)
        ERROR("send error : %s", zmq_strerror(errno));
    zmq_msg_close(&pid_msg);
}

static int __open_zmq_socket(void *context, pims_ipc_svc_t *ipc_svc)
{
    char *path = NULL;
    int ret = -1;
    int i = 0;

    void *router = zmq_socket(context, ZMQ_ROUTER);
    if (!router)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        return -1;
    }
    path = g_strdup_printf("ipc://%s", ipc_svc->service);
    if (zmq_bind(router, path) != 0)
    {
        ERROR("bind error : %s", zmq_strerror(errno));
        zmq_close(router);
        return -1;
    }
    g_free(path);

    ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
    ret = chmod(ipc_svc->service, ipc_svc->mode);

    void *worker = zmq_socket(context, ZMQ_ROUTER);
    if (!worker)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        zmq_close(router);
        return -1;
    }
    path = g_strdup_printf("inproc://%s-%s", ipc_svc->service, PIMS_IPC_DEALER_PATH);
    if (zmq_bind(worker, path) != 0)
    {
        ERROR("bind error : %s", zmq_strerror(errno));
        zmq_close(router);
        zmq_close(worker);
        return -1;
    }
    g_free(path);

    void *manager = zmq_socket(context, ZMQ_ROUTER);
    if (!manager)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        zmq_close(router);
        zmq_close(worker);
        return -1;
    }
    path = g_strdup_printf("inproc://%s-%s", ipc_svc->service, PIMS_IPC_MANAGER_PATH);
    if (zmq_bind(manager, path) != 0)
    {
        ERROR("bind error : %s", zmq_strerror(errno));
        zmq_close(router);
        zmq_close(worker);
        zmq_close(manager);
        return -1;
    }
    g_free(path);
    
    void *monitor = zmq_socket(context, ZMQ_PAIR);
    if (!monitor)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        zmq_close(router);
        zmq_close(worker);
        zmq_close(manager);
        return -1;
    }
    path = g_strdup_printf("inproc://%s-%s", ipc_svc->service, PIMS_IPC_MONITOR2_PATH);
    if (zmq_bind(monitor, path) != 0)
    {
        ERROR("bind error : %s", zmq_strerror(errno));
        zmq_close(router);
        zmq_close(worker);
        zmq_close(manager);
        zmq_close(monitor);
        return -1;
    }
    g_free(path);

    ipc_svc->context = context;
    ipc_svc->router = router;
    ipc_svc->worker = worker;
    ipc_svc->manager = manager;
    ipc_svc->monitor = monitor;

    path = g_strdup_printf("%s-%s", ipc_svc->service, PIMS_IPC_MONITOR_PATH);
    ret = _server_socket_init(path, ipc_svc->group, ipc_svc->mode, __client_closed_cb, ipc_svc);
    ASSERT(ret != -1);
    g_free(path);

    // launch worker threads in advance
    for (i = 0; i < ipc_svc->workers_max_count; i++)
        __launch_worker(__worker_loop, context);

    return 0;
}

static void __close_zmq_socket(pims_ipc_svc_t *ipc_svc)
{
    zmq_close(ipc_svc->router);
    zmq_close(ipc_svc->worker);
    zmq_close(ipc_svc->manager);
    zmq_close(ipc_svc->monitor);
}

static int __open_zmq_socket_for_publish(void *context, pims_ipc_svc_for_publish_t *ipc_svc)
{
    char *path = NULL;
    int ret = -1;

    ipc_svc->context = context;
    void *publisher = NULL;
    publisher = zmq_socket(context, ZMQ_PUB);
    if (!publisher)
    {
        ERROR("socket error : %s", zmq_strerror(errno));
        return -1;
    }

    path = g_strdup_printf("ipc://%s", ipc_svc->service);
    if (zmq_bind(publisher, path) != 0)
    {
        ERROR("bind error : %s", zmq_strerror(errno));
        zmq_close(publisher);
        return -1;
    }
    g_free(path);

    ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
    ret = chmod(ipc_svc->service, ipc_svc->mode);

    ipc_svc->context = context;
    ipc_svc->publisher = publisher;

    return 0;
}

static void __close_zmq_socket_for_publish(pims_ipc_svc_for_publish_t *ipc_svc)
{
    zmq_close(ipc_svc->publisher);
}

static void* __main_loop(void *args)
{
    char *path = NULL;
    int ret = -1;
	pims_ipc_svc_t *ipc_svc = (pims_ipc_svc_t*)args;

    void *monitor_peer = zmq_socket(ipc_svc->context, ZMQ_PAIR);
    ASSERT(monitor_peer);
    
    path = g_strdup_printf("inproc://%s-%s", ipc_svc->service, PIMS_IPC_MONITOR2_PATH);
    ret = zmq_connect(monitor_peer, path);
    ASSERT(ret == 0);
    g_free(path);

    // poll all sockets
    while (1)
    {
        zmq_pollitem_t items[] = {
            {ipc_svc->router, 0, ZMQ_POLLIN, 0},
            {ipc_svc->worker, 0, ZMQ_POLLIN, 0},
            {ipc_svc->manager, 0, ZMQ_POLLIN, 0},
            {monitor_peer, 0, ZMQ_POLLIN, 0}
        };

        if (zmq_poll(items, 4, -1) == -1)
        {
            if (errno == EINTR)
                continue;

            ERROR("poll error : %s", zmq_strerror(errno));
            break;
        }

        if (items[0].revents & ZMQ_POLLIN)
        {
            __process_router_event(ipc_svc->context, ipc_svc->router, ipc_svc->worker, FALSE);
        }

        if (items[1].revents & ZMQ_POLLIN)
        {
            __process_worker_event(ipc_svc->context, ipc_svc->worker, ipc_svc->router);
        }

        if (items[2].revents & ZMQ_POLLIN)
        {
            __process_manager_event(ipc_svc->context, ipc_svc->manager);
            if (ipc_svc->requests)
                __process_router_event(ipc_svc->context, ipc_svc->router, ipc_svc->worker, TRUE);
        }

        if (items[3].revents & ZMQ_POLLIN)
        {
            __process_monitor_event(ipc_svc->context, monitor_peer, ipc_svc->manager);
        }
    }
    
    zmq_close(monitor_peer);

    return NULL;
}

API void pims_ipc_svc_run_main_loop(GMainLoop* loop)
{
    int retval = -1;
    GMainLoop* main_loop = loop;

    if(main_loop == NULL) {
        main_loop = g_main_loop_new(NULL, FALSE);
    }

    void *context = zmq_init(1);
    ASSERT (context != NULL);

    if (_g_singleton_for_publish)
    {
        retval = __open_zmq_socket_for_publish(context, _g_singleton_for_publish);
        ASSERT(retval == 0);
    }

    if (_g_singleton)
    {
        retval = __open_zmq_socket(context, _g_singleton);
        ASSERT(retval == 0);
    }

    __launch_worker(__main_loop, _g_singleton);

    g_main_loop_run(main_loop);

    if (_g_singleton)
    {
        __close_zmq_socket(_g_singleton);
    }

    if (_g_singleton_for_publish)
    {
        __close_zmq_socket_for_publish(_g_singleton_for_publish);
    }

    if (zmq_term(context) == -1)
        WARNING("term error : %s", zmq_strerror(errno));
}
