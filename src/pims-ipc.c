/*
 * PIMS IPC
 *
 * Copyright (c) 2012 - 2013 Samsung Electronics Co., Ltd. All rights reserved.
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
#include <time.h>
#include <glib.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>

#include <pims-internal.h>
#include <pims-socket.h>
#include <pims-debug.h>
#include <pims-ipc-data.h>
#include <pims-ipc.h>

#define GET_CALL_SEQUNECE_NO(handle, sequence_no) do {\
    sequence_no = ++((handle)->call_sequence_no);\
} while (0)

static pthread_mutex_t __gmutex = PTHREAD_MUTEX_INITIALIZER;

typedef enum
{
    PIMS_IPC_CALL_STATUS_READY = 0,
    PIMS_IPC_CALL_STATUS_IN_PROGRESS
} pims_ipc_call_status_e;

typedef enum
{
    PIMS_IPC_MODE_REQ = 0,
    PIMS_IPC_MODE_SUB
} pims_ipc_mode_e;

typedef struct
{
    pid_t pid;
    void *context;
    unsigned int ref_cnt;
    GList *subscribe_handles;
} pims_ipc_context_t;

static pims_ipc_context_t *_g_singleton = NULL;

typedef struct
{
    pims_ipc_subscribe_cb callback;
    void * user_data;
} pims_ipc_cb_t;

typedef struct
{
    int fd;
    void *requester;
    char *service;
    char *id;
    GIOChannel *async_channel;
    guint async_source_id;
    pims_ipc_call_status_e call_status;
    unsigned int call_sequence_no;
    pims_ipc_call_async_cb call_async_callback;
    void *call_async_userdata;
    GHashTable *subscribe_cb_table;
} pims_ipc_t;

#define PIMS_IPC_SOCKET_BUFFER_SIZE     256
static inline int __pims_zmq_msg_recv_by_handle(zmq_msg_t *msg, pims_ipc_t *handle, int flags)
{
    int ret = -1;

    while (1)
    {
        zmq_pollitem_t items[] = {
            {handle->requester, 0, ZMQ_POLLIN, 0},
            {NULL, handle->fd, ZMQ_POLLIN, 0}
        };

        if (zmq_poll(items, 2, -1) == -1)
        {
            if (errno == EINTR)
                continue;
            
            ERROR("poll error : %s", zmq_strerror(errno));
            break;
        }

        if (items[0].revents & ZMQ_POLLIN)
        {
            ret = zmq_msg_recv(msg, handle->requester, flags);
            if (ret == -1 && errno == EINTR)
                continue;
            break;
        }

        if (items[1].revents & ZMQ_POLLIN)
        {
            char buffer[PIMS_IPC_SOCKET_BUFFER_SIZE] = "";
           
            memset(buffer, 0x00, PIMS_IPC_SOCKET_BUFFER_SIZE);
            ret = read(handle->fd, (char *)buffer, PIMS_IPC_SOCKET_BUFFER_SIZE-1);
            ASSERT(ret <= 0);
            
            close(handle->fd);
            handle->fd = -1;

            if (handle->requester)
                zmq_close(handle->requester);
            handle->requester = NULL;

            errno = ETERM;
            ret = -1;
            break;
        }
    }

    return ret;
}

static void __pims_ipc_free_handle(pims_ipc_t *handle)
{
    pthread_mutex_lock(&__gmutex);

    g_free(handle->id);
    g_free(handle->service);

    if (handle->requester)
        zmq_close(handle->requester);

    if (handle->fd != -1)
        close(handle->fd);
    
    if (handle->async_channel)
    {
        // remove a subscriber handle from the golbal list
        if (_g_singleton)
        {
            _g_singleton->subscribe_handles = g_list_remove(_g_singleton->subscribe_handles, handle);
            VERBOSE("the count of subscribe handles = %d", g_list_length(_g_singleton->subscribe_handles));
        }

        g_source_remove(handle->async_source_id);
        g_io_channel_unref(handle->async_channel);
    }

    if (handle->subscribe_cb_table)
        g_hash_table_destroy(handle->subscribe_cb_table);

    g_free(handle);

    if (_g_singleton && --_g_singleton->ref_cnt <= 0)
    {
        if (zmq_term(_g_singleton->context) == -1)
        {
            WARNING("term error : %s", zmq_strerror(errno));
        }
        g_free(_g_singleton);
        _g_singleton = NULL;
    }
    
    pthread_mutex_unlock(&__gmutex);
}

static int __pims_ipc_receive_for_subscribe(pims_ipc_t *handle)
{
    gboolean is_valid = FALSE;
    int64_t more = 0;
    pims_ipc_data_h dhandle = NULL;
    pims_ipc_cb_t *cb_data = NULL;
   
    zmq_msg_t call_id_msg;
    zmq_msg_t data_msg;

    zmq_msg_init(&call_id_msg);
    zmq_msg_init(&data_msg);

    do {
        // recv call id 
        if (__pims_zmq_msg_recv_by_handle(&call_id_msg, handle, 0) == -1)
        {
            ERROR("recv error : %s", zmq_strerror(errno));
            break;
        }

        // find a callback by a call id
        cb_data = (pims_ipc_cb_t*)g_hash_table_lookup(handle->subscribe_cb_table, zmq_msg_data(&call_id_msg));

        size_t more_size = sizeof(more);
        zmq_getsockopt(handle->requester, ZMQ_RCVMORE, &more, &more_size);
        if (more)
        {
            if (__pims_zmq_msg_recv_by_handle(&data_msg, handle, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            } 
            
            if (cb_data == NULL)
            {
                VERBOSE("unable to find %s", (char*)zmq_msg_data(&call_id_msg));
                is_valid = TRUE;
                break;
            }

            dhandle = pims_ipc_data_unmarshal_with_zmq(&data_msg);
            if (dhandle == NULL)
            {
                ERROR("unmarshal error");
                break;
            }

            cb_data->callback((pims_ipc_h)handle, dhandle, cb_data->user_data);
        }

        is_valid = TRUE;
    } while (0);

    zmq_msg_close(&call_id_msg);
    zmq_msg_close(&data_msg);
    if (dhandle)
        pims_ipc_data_destroy(dhandle);

    if (is_valid == FALSE)
        return -1;
    return 0;
}

static gboolean __pims_ipc_subscribe_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
    pims_ipc_t *handle = (pims_ipc_t *)data;
    uint32_t zmq_events = 0;
    size_t opt_len = 0;
    int rc = 0;

    VERBOSE("");

    pthread_mutex_lock(&__gmutex);

    // check if a subscriber handle is exists
    if (_g_singleton == NULL || g_list_find(_g_singleton->subscribe_handles, handle) == NULL)
    {
        DEBUG("No such handle that ID is %p", handle);
        pthread_mutex_unlock(&__gmutex);
        return FALSE;
    }
    
    opt_len = sizeof(uint32_t);
    while (1)
    {
        rc = zmq_getsockopt(handle->requester, ZMQ_EVENTS, &zmq_events, &opt_len);
        ASSERT(rc == 0);
        if (ZMQ_POLLIN & zmq_events) {
            __pims_ipc_receive_for_subscribe(handle);
        }
        else
        {
            break;
        }
    }

    pthread_mutex_unlock(&__gmutex);

    return TRUE;
}

static unsigned int __get_global_sequence_no()
{
    static unsigned int __gsequence_no = 0xffffffff;

    if (__gsequence_no == 0xffffffff)
        __gsequence_no = (unsigned int)time(NULL);
    else
        __gsequence_no++;
    return __gsequence_no;
}

static pims_ipc_h __pims_ipc_create(char *service, pims_ipc_mode_e mode)
{
    pims_ipc_context_t *ghandle = NULL;
    pims_ipc_t *handle = NULL;
    pid_t pid = 0;
    void *context = NULL;
    void *requester = NULL;
    char *path = NULL;
    gboolean is_ok = FALSE;
        
    pthread_mutex_lock(&__gmutex);

    do {
        if (_g_singleton == NULL)
        {
            ghandle = g_new0(pims_ipc_context_t, 1);
            if (ghandle == NULL)
            {
                ERROR("Failed to allocation");
                break;
            }
            
            pid = getpid();
            ghandle->pid = pid;
            VERBOSE("The PID of the current process is %d.", pid);

            context = zmq_init(1);
            if (!context)
            {
                ERROR("init error : %s", zmq_strerror(errno));
                break;
            }
            ghandle->context = context;
            ghandle->ref_cnt = 1;
            _g_singleton = ghandle;
        }
        else
        {
            ghandle = _g_singleton;
            ghandle->ref_cnt++;
            pid = ghandle->pid;
            context = ghandle->context;
        }

        VERBOSE("Create %d th..", ghandle->ref_cnt);

        handle = g_new0(pims_ipc_t, 1);
        if (handle == NULL)
        {
            ERROR("Failed to allocation");
            break;
        }
        handle->fd = -1;

        handle->service = g_strdup(service);
        handle->id = g_strdup_printf("%x:%x", pid, __get_global_sequence_no());
        
        if (mode == PIMS_IPC_MODE_REQ)
        {
            path = g_strdup_printf("%s-%s", handle->service, PIMS_IPC_MONITOR_PATH);
            handle->fd = _client_socket_init(path, handle->id);
            if (handle->fd == -1)
            {
                g_free(path);
                break;
            }
            g_free(path);

            requester = zmq_socket(context, ZMQ_REQ);
            if (!requester)
            {
                ERROR("socket error : %s", zmq_strerror(errno));
                break;
            }
            if (zmq_setsockopt(requester, ZMQ_IDENTITY, handle->id, strlen(handle->id) + 1) != 0)
            {
                ERROR("setsockopt error : %s", zmq_strerror(errno));
                break;
            }
            handle->requester = requester;

            path = g_strdup_printf("ipc://%s", handle->service);
            if (zmq_connect(requester, path) != 0)
            {
                ERROR("connect error : %s", zmq_strerror(errno));
                g_free(path);
                break;
            }
            g_free(path);

            handle->call_sequence_no = (unsigned int)time(NULL);
            if (pims_ipc_call(handle, PIMS_IPC_MODULE_INTERNAL, PIMS_IPC_FUNCTION_CREATE, NULL, NULL) != 0)
            {
                WARNING("pims_ipc_call(PIMS_IPC_FUNCTION_CREATE) failed");
            }
        }
        else
        {
            requester = zmq_socket(context, ZMQ_SUB);
            if (!requester)
            {
                ERROR("socket error : %s", zmq_strerror(errno));
                break;
            }
            if (zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0) != 0)
            {
                ERROR("setsockopt error : %s", zmq_strerror(errno));
                break;
            }
            handle->requester = requester;

            path = g_strdup_printf("ipc://%s", handle->service);
            if (zmq_connect(requester, path) != 0)
            {
                ERROR("connect error : %s", zmq_strerror(errno));
                g_free(path);
                break;
            }
            g_free(path);

            int fd = -1;
            size_t opt_len = sizeof(int);
            int rc = zmq_getsockopt(handle->requester, ZMQ_FD, &fd, &opt_len);
            ASSERT(rc == 0);

            handle->async_channel = g_io_channel_unix_new(fd);
            if (!handle->async_channel)
            {
                ERROR("g_io_channel_unix_new error");
                break;
            }

            guint source_id = 0;
            source_id = g_io_add_watch(handle->async_channel, G_IO_IN, __pims_ipc_subscribe_handler, handle);
            handle->async_source_id = source_id;
            handle->subscribe_cb_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            ASSERT(handle->subscribe_cb_table);

            // add a subscriber handle to the global list
            ghandle->subscribe_handles = g_list_append(ghandle->subscribe_handles, handle);
            VERBOSE("the count of subscribe handles = %d", g_list_length(ghandle->subscribe_handles));
        }

        is_ok = TRUE;
        VERBOSE("A new handle is created : %s, %s", handle->service, handle->id);
    } while(0);

    pthread_mutex_unlock(&__gmutex);

    if (FALSE == is_ok)
    {
        if (handle)
        {
            __pims_ipc_free_handle(handle);
            handle = NULL;
        }
    }

    return handle;
}

API pims_ipc_h pims_ipc_create(char *service)
{
    return __pims_ipc_create(service, PIMS_IPC_MODE_REQ);
}

API pims_ipc_h pims_ipc_create_for_subscribe(char *service)
{
    return __pims_ipc_create(service, PIMS_IPC_MODE_SUB);
}

static void __pims_ipc_destroy(pims_ipc_h ipc, pims_ipc_mode_e mode)
{
    pims_ipc_t *handle = (pims_ipc_t *)ipc;

    if (mode == PIMS_IPC_MODE_REQ)
    {
        if (pims_ipc_call(handle, PIMS_IPC_MODULE_INTERNAL, PIMS_IPC_FUNCTION_DESTROY, NULL, NULL) != 0)
        {
            WARNING("pims_ipc_call(PIMS_IPC_FUNCTION_DESTROY) failed");
        }
    }

    __pims_ipc_free_handle(handle);
}

API void pims_ipc_destroy(pims_ipc_h ipc)
{
    __pims_ipc_destroy(ipc, PIMS_IPC_MODE_REQ);
}

API void pims_ipc_destroy_for_subscribe(pims_ipc_h ipc)
{
    __pims_ipc_destroy(ipc, PIMS_IPC_MODE_SUB);
}

static void __pims_ipc_data_free_cb(void *data, void *hint)
{
    if (hint)
        g_free(hint);
}

static int __pims_ipc_send(pims_ipc_t *handle, char *module, char *function, pims_ipc_h data_in)
{
    gboolean is_valid = FALSE;
    unsigned int sequence_no = 0;
    gchar *call_id = PIMS_IPC_MAKE_CALL_ID(module, function);

    // init messages
    zmq_msg_t sequence_no_msg;
    zmq_msg_t call_id_msg;
    zmq_msg_t data_in_msg;

    zmq_msg_init_size(&sequence_no_msg, sizeof(unsigned int));
    GET_CALL_SEQUNECE_NO(handle, sequence_no);
    memcpy(zmq_msg_data(&sequence_no_msg), &(sequence_no), sizeof(unsigned int));

    zmq_msg_init_data(&call_id_msg, call_id, strlen(call_id) + 1, __pims_ipc_data_free_cb, call_id);
    VERBOSE("call id = %s", (char*)zmq_msg_data(&call_id_msg));

    zmq_msg_init(&data_in_msg);

    do {
        // send sequence no
        if (_pims_zmq_msg_send(&sequence_no_msg, handle->requester, ZMQ_SNDMORE) == -1)
        {
            ERROR("send error : %s", zmq_strerror(errno));
            break;
        }

        if (data_in == NULL)
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, handle->requester, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }
        else
        {
            // send call id
            if (_pims_zmq_msg_send(&call_id_msg, handle->requester, ZMQ_SNDMORE) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }

            // marshal data
            if (pims_ipc_data_marshal_with_zmq(data_in, &data_in_msg) != 0)
            {
                ERROR("marshal error");
                break;
            }

            VERBOSE("the size of sending data = %d", zmq_msg_size(&data_in_msg));
            
            // send data
            if (_pims_zmq_msg_send(&data_in_msg, handle->requester, 0) == -1)
            {
                ERROR("send error : %s", zmq_strerror(errno));
                break;
            }
        }

        is_valid = TRUE;
    } while (0);

    zmq_msg_close(&sequence_no_msg);
    zmq_msg_close(&call_id_msg);
    zmq_msg_close(&data_in_msg);

    if (is_valid == FALSE)
        return -1;
    return 0;
}

static int __pims_ipc_receive(pims_ipc_t *handle, pims_ipc_data_h *data_out)
{
    gboolean is_ok = FALSE;
    gboolean is_valid = FALSE;
    int64_t more = 0;
    pims_ipc_data_h dhandle = NULL;
    unsigned int sequence_no = 0;
   
    zmq_msg_t sequence_no_msg;
    zmq_msg_t call_id_msg;
    zmq_msg_t data_out_msg;

    while (1)
    {
        is_valid = FALSE;
        more = 0;

        zmq_msg_init(&sequence_no_msg);
        zmq_msg_init(&call_id_msg);
        zmq_msg_init(&data_out_msg);

        do {
            // recv sequence no
            if (__pims_zmq_msg_recv_by_handle(&sequence_no_msg, handle, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }
            memcpy(&sequence_no, zmq_msg_data(&sequence_no_msg), sizeof(unsigned int));

            // recv call id 
            if (__pims_zmq_msg_recv_by_handle(&call_id_msg, handle, 0) == -1)
            {
                ERROR("recv error : %s", zmq_strerror(errno));
                break;
            }

            size_t more_size = sizeof(more);
            zmq_getsockopt(handle->requester, ZMQ_RCVMORE, &more, &more_size);
            if (more)
            {
                if (__pims_zmq_msg_recv_by_handle(&data_out_msg, handle, 0) == -1)
                {
                    ERROR("recv error : %s", zmq_strerror(errno));
                    break;
                }
                dhandle = pims_ipc_data_unmarshal_with_zmq(&data_out_msg);
                if (dhandle == NULL)
                {
                    ERROR("unmarshal error");
                    break;
                }

                if (sequence_no == handle->call_sequence_no)
                {
                    if (data_out != NULL)
                        *data_out = dhandle;
                    is_ok = TRUE;
                }
                else
                {
                    pims_ipc_data_destroy(dhandle);
                    DEBUG("received an mismatched response (%x:%x)", handle->call_sequence_no, sequence_no);
                }
            }
            else
            {
                if (sequence_no == handle->call_sequence_no)
                    is_ok = TRUE;
            }

            is_valid = TRUE;
        } while (0);

        zmq_msg_close(&sequence_no_msg);
        zmq_msg_close(&call_id_msg);
        zmq_msg_close(&data_out_msg);

        if (is_ok)
            return 0;

        if (is_valid == FALSE)
            return -1;
    }

    return -1;
}

API int pims_ipc_call(pims_ipc_h ipc, char *module, char *function, pims_ipc_data_h data_in,
                      pims_ipc_data_h *data_out)
{
    pims_ipc_t *handle = (pims_ipc_t *)ipc;


    if (ipc == NULL)
    {
        ERROR("invalid handle : %p", ipc);
        return -1;
    }

    if (!module || !function)
    {
        ERROR("invalid argument");
        return -1;
    }

    if (handle->call_status != PIMS_IPC_CALL_STATUS_READY)
    {
        ERROR("the previous call is in progress : %p", ipc);
        return -1;
    }

    if (__pims_ipc_send(handle, module, function, data_in) != 0)
    {
        return -1;
    }

    if (__pims_ipc_receive(handle, data_out) != 0)
    {
        return -1;
    }
    
    return 0;
}

static gboolean __pims_ipc_call_async_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
    pims_ipc_t *handle = (pims_ipc_t *)data;
    uint32_t zmq_events = 0;
    size_t opt_len = 0;
    int rc = 0;

    VERBOSE("");

    opt_len = sizeof(uint32_t);
    while (1)
    {
        rc = zmq_getsockopt(handle->requester, ZMQ_EVENTS, &zmq_events, &opt_len);
        ASSERT(rc == 0);
        if (ZMQ_POLLIN & zmq_events) {
            pims_ipc_data_h dhandle = NULL;
            if (__pims_ipc_receive(handle, &dhandle) == 0)
            {
                VERBOSE("call status = %d", handle->call_status);
                if (handle->call_status != PIMS_IPC_CALL_STATUS_IN_PROGRESS)
                {
                    pims_ipc_data_destroy(dhandle);
                }
                else
                {
                    handle->call_status = PIMS_IPC_CALL_STATUS_READY;
                    handle->call_async_callback((pims_ipc_h)handle, dhandle, handle->call_async_userdata);
                    pims_ipc_data_destroy(dhandle);
                }
            }
        }
        else
        {
            break;
        }
    }

    return FALSE;
}

API int pims_ipc_call_async(pims_ipc_h ipc, char *module, char *function, pims_ipc_data_h data_in,
                            pims_ipc_call_async_cb callback, void *userdata)
{
    pims_ipc_t *handle = (pims_ipc_t *)ipc;
    guint source_id = 0;

    if (ipc == NULL)
    {
        ERROR("invalid handle : %p", ipc);
        return -1;
    }

    if (!module || !function || !callback)
    {
        ERROR("invalid argument");
        return -1;
    }

    if (handle->call_status != PIMS_IPC_CALL_STATUS_READY)
    {
        ERROR("the previous call is in progress : %p", ipc);
        return -1;
    }

    handle->call_status = PIMS_IPC_CALL_STATUS_IN_PROGRESS;
    handle->call_async_callback = callback;
    handle->call_async_userdata = userdata;

    // add a callback for GIOChannel
    if (!handle->async_channel)
    {
        int fd = -1;
        size_t opt_len = sizeof(int);
        int rc = zmq_getsockopt(handle->requester, ZMQ_FD, &fd, &opt_len);
        ASSERT(rc == 0);

        handle->async_channel = g_io_channel_unix_new(fd);
        if (!handle->async_channel)
        {
            ERROR("g_io_channel_unix_new error");
            return -1;
        }
    }
    
    source_id = g_io_add_watch(handle->async_channel, G_IO_IN, __pims_ipc_call_async_handler, handle);
    handle->async_source_id = source_id;

    if (__pims_ipc_send(handle, module, function, data_in) != 0)
    {
        g_source_remove(source_id);
        return -1;
    }
    
    uint32_t zmq_events = 0;
    size_t opt_len = sizeof(uint32_t);
    int rc = 0;
    rc = zmq_getsockopt(handle->requester, ZMQ_EVENTS, &zmq_events, &opt_len);
    ASSERT(rc == 0);

    return 0;
}

API bool pims_ipc_is_call_in_progress(pims_ipc_h ipc)
{
    pims_ipc_t *handle = (pims_ipc_t *)ipc;

    if (ipc == NULL)
    {
        ERROR("invalid handle : %p", ipc);
        return false;
    }

    if (handle->call_status == PIMS_IPC_CALL_STATUS_IN_PROGRESS)
        return true;
    else
        return false;
}

API int pims_ipc_subscribe(pims_ipc_h ipc, char *module, char *event, pims_ipc_subscribe_cb callback, void *userdata)
{
    gchar *call_id = NULL;
    pims_ipc_cb_t *cb_data = NULL;
    pims_ipc_t *handle = (pims_ipc_t *)ipc;

    if (ipc == NULL || handle->subscribe_cb_table == NULL)
    {
        ERROR("invalid handle : %p", ipc);
        return -1;
    }

    if (!module || !event || !callback)
    {
        ERROR("invalid argument");
        return -1;
    }

    cb_data = g_new0(pims_ipc_cb_t, 1);
    call_id = PIMS_IPC_MAKE_CALL_ID(module, event);

    VERBOSE("subscribe cb id[%s]", call_id);
    cb_data->callback = callback;
    cb_data->user_data = userdata;
    g_hash_table_insert(handle->subscribe_cb_table, call_id, cb_data);

    return 0;
}

API int pims_ipc_unsubscribe(pims_ipc_h ipc, char *module, char *event)
{
    gchar *call_id = NULL;
    pims_ipc_t *handle = (pims_ipc_t *)ipc;

    if (ipc == NULL || handle->subscribe_cb_table == NULL)
    {
        ERROR("invalid handle : %p", ipc);
        return -1;
    }

    if (!module || !event)
    {
        ERROR("invalid argument");
        return -1;
    }

    call_id = PIMS_IPC_MAKE_CALL_ID(module, event);

    VERBOSE("unsubscribe cb id[%s]", call_id);

    if (g_hash_table_remove(handle->subscribe_cb_table, call_id) != TRUE)
    {
        ERROR("g_hash_table_remove error");
        g_free(call_id);
        return -1;
    }

    g_free(call_id);
    return 0;
}
