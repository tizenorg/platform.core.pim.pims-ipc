/*
 * PIMS IPC
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <pims-ipc.h>
#include <pims-ipc-svc.h>
#include <pims-ipc-data.h>

#define THREAD_COUNT    1
#define REPEAT_COUNT    100
#define BUFFER_SIZE 1024
#define PLUS_TIME(a, b, c) do {\
    a.tv_sec = b.tv_sec + c.tv_sec;\
    a.tv_usec = b.tv_usec + c.tv_usec;\
    if (a.tv_usec >= 1000000) {\
        a.tv_sec++;\
        a.tv_usec -= 1000000;\
    }\
} while (0)

#define MINUS_TIME(a, b, c) do {\
    a.tv_sec = b.tv_sec - c.tv_sec;\
    a.tv_usec = b.tv_usec - c.tv_usec;\
    if (a.tv_usec < 0) {\
        a.tv_sec--;\
        a.tv_usec += 1000000;\
    }\
} while (0)

typedef struct {
    int thread_count;
    int repeat_count;
    int message_size;
    pims_ipc_h ipc;
} test_arg_t;

static gboolean __is_async = FALSE;
static gboolean __is_publish = FALSE;

void test_function(pims_ipc_h ipc, pims_ipc_data_h indata, pims_ipc_data_h *outdata, void *userdata)
{
    unsigned int size = 0;
    char *str = NULL;

    if (indata)
    {
        str = (char*)pims_ipc_data_get(indata, &size);
        if (!str)
        {
            printf("pims_ipc_data_get error\n");
            return;
        }
#if 0
        str = (char*)pims_ipc_data_get(indata, &size);
        if (!str)
        {
            printf("pims_ipc_data_get error\n");
            return;
        }
        printf("%s\n", str);
        str = (char*)pims_ipc_data_get(indata, &size);
        if (!str)
        {
            printf("pims_ipc_data_get error\n");
            return;
        }
        printf("%s\n", str);
#endif
    }

    if (str && outdata)
    {
        *outdata = pims_ipc_data_create(0);
        if (!*outdata)
        {
            printf("pims_ipc_data_create error\n");
            return;
        }
        if (pims_ipc_data_put(*outdata, str, strlen(str) + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            pims_ipc_data_destroy(*outdata);
            *outdata = NULL;
            return;
        }
#if 0
        if (pims_ipc_data_put(*outdata, "welcome", strlen("welcome") + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            pims_ipc_data_destroy(*outdata);
            *outdata = NULL;
            return;
        }
        if (pims_ipc_data_put(*outdata, "to the jungle", strlen("to the jungle") + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            pims_ipc_data_destroy(*outdata);
            *outdata = NULL;
            return;
        }
#endif
    }
}

static gboolean publish_callback(gpointer data)
{
    pims_ipc_data_h indata = NULL;

    indata = pims_ipc_data_create(0);
    if (!indata)
    {
        printf("pims_ipc_data_create error\n");
        return false;
    }
    if (pims_ipc_data_put(indata, "publish test", strlen("publish test") + 1) != 0)
    {
        printf("pims_ipc_data_put error\n");
        return false;
    }
    if (pims_ipc_svc_publish("test_module", "publish", indata) != 0)
    {
        printf("pims_ipc_svc_publish error\n");
        return false;
    }

    if (indata)
        pims_ipc_data_destroy(indata);

    return true;
}

int server_main()
{

    pims_ipc_svc_init("pims-ipc-test", getuid(), 0660);

    if (pims_ipc_svc_register("test_module", "test_function", test_function, NULL) != 0)
    {
        printf("pims_ipc_svc_register error\n");
        return -1;
    }

    if (__is_publish)
    {
        pims_ipc_svc_init_for_publish("pims-ipc-pub-test", getuid(), 0660);
        g_timeout_add_seconds(3, publish_callback, NULL);
    }

    pims_ipc_svc_run_main_loop(NULL);

    pims_ipc_svc_deinit();

    return 0;
}

static gboolean call_async_idler_callback(gpointer data);

static void call_async_callback(pims_ipc_h ipc, pims_ipc_data_h data_out, void *userdata)
{
    unsigned int size = 0;
    char *str = NULL;

    printf("(%x) call_async_callback(%p)", (unsigned int)pthread_self(), ipc);
    if (data_out)
    {
        str = (char*)pims_ipc_data_get(data_out, &size);
        if (!str)
        {
            printf("pims_ipc_data_get error\n");
        }
        else
        {
            printf("pims_ipc_data_get(%s) success\n", str);
        }
    }

    sleep(1);

    pims_ipc_data_h indata = NULL;
    pims_ipc_data_h outdata = NULL;

    indata = pims_ipc_data_create(0);
    if (!indata)
    {
        printf("pims_ipc_data_create error\n");
        return;
    }
    if (pims_ipc_data_put(indata, "hello world", strlen("hello world") + 1) != 0)
    {
        printf("pims_ipc_data_put error\n");
        return;
    }

    if (pims_ipc_call(ipc, "test_module", "test_function", indata, &outdata) != 0)
    {
        printf("pims_ipc_call error\n");
        return;
    }
    
    if (indata)
        pims_ipc_data_destroy(indata);
    if (outdata)
        pims_ipc_data_destroy(outdata);

    sleep(1);

    call_async_idler_callback(ipc);
}

static gboolean call_async_idler_callback(gpointer data)
{
    pims_ipc_data_h indata = NULL;
    pims_ipc_data_h outdata = NULL;
    pims_ipc_h ipc = data;
    bool ret = false;

    indata = pims_ipc_data_create(0);
    if (!indata)
    {
        printf("pims_ipc_data_create error\n");
        return FALSE;
    }
    if (pims_ipc_data_put(indata, "hello world", strlen("hello world") + 1) != 0)
    {
        printf("pims_ipc_data_put error\n");
        return FALSE;
    }

    printf("(%x) call_async_idler_callback(%p)\n", (unsigned int)pthread_self(), ipc);
    ret = pims_ipc_is_call_in_progress(ipc);
    printf("(%x) before async call : pims_ipc_is_call_in_progress(%p) = %d\n", (unsigned int)pthread_self(), ipc, ret);
    if (pims_ipc_call_async(ipc, "test_module", "test_function", indata, call_async_callback, NULL) != 0)
    {
        printf("pims_ipc_call_async error\n");
        return FALSE;
    }
    ret = pims_ipc_is_call_in_progress(ipc);
    printf("(%x) after async call : pims_ipc_is_call_in_progress(%p) = %d\n", (unsigned int)pthread_self(), ipc, ret);

    if (pims_ipc_call(ipc, "test_module", "test_function", indata, &outdata) != 0)
    {
        printf("pims_ipc_call error during async-call\n");
        return FALSE;
    }
    if (indata)
        pims_ipc_data_destroy(indata);
    if (outdata)
        pims_ipc_data_destroy(outdata);

    return FALSE;
}

int client_main(pims_ipc_h ipc, int repeat_count, int message_size)
{
    pims_ipc_data_h indata = NULL;
    pims_ipc_data_h outdata = NULL;
    int retval = -1;
    unsigned int size = 0;
    char *str = NULL;
    int i = 0;
    struct timeval start_tv = {0, 0};
    struct timeval end_tv = {0, 0};
    struct timeval diff_tv = {0, 0};
    struct timeval prev_sum_tv = {0, 0};
    struct timeval sum_tv = {0, 0};
    int count = 0;
    
    char *buffer = g_malloc0(message_size + 1);

    for (i = 0; i < message_size; i++)
        buffer[i] = 'a';
    buffer[i] = 0;

    for (i = 0; i < repeat_count; i++)
    {
        gettimeofday(&start_tv, NULL);
        indata = pims_ipc_data_create(0);
        if (!indata)
        {
            printf("pims_ipc_data_create error\n");
            break;
        }
        if (pims_ipc_data_put(indata, buffer, strlen(buffer) + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            break;
        }
#if 0
        if (pims_ipc_data_put(indata, "hellow", strlen("hellow") + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            break;
        }
        if (pims_ipc_data_put(indata, "world", strlen("world") + 1) != 0)
        {
            printf("pims_ipc_data_put error\n");
            break;
        }
#endif
        if (pims_ipc_call(ipc, "test_module", "test_function", indata, &outdata) != 0)
        {
            printf("pims_ipc_call error\n");
            break;
        }
        pims_ipc_data_destroy(indata);
        indata = NULL;
        if (outdata)
        {
            str = (char*)pims_ipc_data_get(outdata, &size);
            if (!str)
            {
                printf("pims_ipc_data_get error\n");
                break;
            }
#if 0
            str = (char*)pims_ipc_data_get(outdata, &size);
            if (!str)
            {
                printf("pims_ipc_data_get error\n");
                break;
            }
            printf("%s\n", str);
            str = (char*)pims_ipc_data_get(outdata, &size);
            if (!str)
            {
                printf("pims_ipc_data_get error\n");
                break;
            }
            printf("%s\n", str);
#endif

            pims_ipc_data_destroy(outdata);
            outdata = NULL;
        }
        gettimeofday(&end_tv, NULL);
        MINUS_TIME(diff_tv, end_tv, start_tv);
        PLUS_TIME(sum_tv, diff_tv, prev_sum_tv);
        prev_sum_tv = sum_tv;
        count++;
        printf("(%x) start[%lu:%lu] end[%lu:%lu] diff[%lu:%lu]\n",
                (unsigned int)pthread_self(),
                start_tv.tv_sec, start_tv.tv_usec,
                end_tv.tv_sec, end_tv.tv_usec,
                diff_tv.tv_sec, diff_tv.tv_usec);
    }

    if (i == repeat_count)
    {
        printf("(%x) sum[%lu:%lu] count[%d]\n",
                (unsigned int)pthread_self(),
                sum_tv.tv_sec, sum_tv.tv_usec, count);
        printf("(%x) avg[%lu:%lu]\n",
                (unsigned int)pthread_self(),
                sum_tv.tv_sec / count, (sum_tv.tv_sec % count * 1000000 + sum_tv.tv_usec) / count);
        retval = 0;
    }

    if (indata)
        pims_ipc_data_destroy(indata);
    if (outdata)
        pims_ipc_data_destroy(outdata);

    if (__is_async)
        g_idle_add(call_async_idler_callback, ipc);

    return retval;
}

static void* __worker_task(void *arg)
{
    test_arg_t *_arg = arg;
    printf("(%x) worker %p, %d, %d\n", (unsigned int)pthread_self(), _arg->ipc, _arg->repeat_count, _arg->message_size);
    client_main(_arg->ipc, _arg->repeat_count, _arg->message_size);
    printf("worker task has been done\n");
    return NULL;
}

static gboolean __launch_worker(gpointer data)
{
    test_arg_t *_arg = data;
    int i = 0;
    GThread **worker_threads = NULL;
    gboolean joinable = FALSE;

    worker_threads = g_new0(GThread*, _arg->thread_count);
    if (!__is_async && !__is_publish)
        joinable = TRUE;

    for (i = 0; i < _arg->thread_count; i++)
    {
        pims_ipc_h ipc = NULL;
        test_arg_t *arg = g_new0(test_arg_t, 1);
        arg->repeat_count = _arg->repeat_count;
        arg->message_size = _arg->message_size;
        ipc = pims_ipc_create("pims-ipc-test");
        if (!ipc)
        {
            printf("pims_ipc_create error\n");
            return -1;
        }
        arg->ipc = ipc;

        worker_threads[i] = g_thread_create(__worker_task, (void*)arg, joinable, NULL);
    }

    if (!__is_async && !__is_publish)
    {
        for (i = 0; i < _arg->thread_count; i++)
        {
            g_thread_join(worker_threads[i]);
        }
    }

    g_free(worker_threads);

    return FALSE;
}

static void subscribe_callback(pims_ipc_h ipc, pims_ipc_data_h data, void *userdata)
{
    unsigned int size = 0;
    char *str = NULL;

    printf("(%x) subscribe_callback(%p)", (unsigned int)pthread_self(), ipc);
    if (data)
    {
        str = (char*)pims_ipc_data_get(data, &size);
        if (!str)
        {
            printf("pims_ipc_data_get error\n");
        }
        else
        {
            printf("pims_ipc_data_get(%s) success\n", str);
        }
    }
}

static void __print_usage(char *prog)
{
    printf("Usage: %s [-r <repeat count> -s <message size> -t <thread count> -a] client|server \n", prog);
}

int main(int argc, char *argv[])
{
    int repeat_count = REPEAT_COUNT;
    int message_size = BUFFER_SIZE;
    int thread_count = THREAD_COUNT;
    int c = 0;
    test_arg_t arg;

    opterr = 0;
    optind = 0;

    g_thread_init(NULL);

    while ((c = getopt(argc, argv, "r:s:t:ap")) != -1)
    {
        switch (c)
        {
            case 'r':
                repeat_count = atoi(optarg);
                break;
            case 's':
                message_size = atoi(optarg);
                break;
            case 't':
                thread_count = atoi(optarg);
                break;
            case 'a':
                __is_async = TRUE;
                break;
            case 'p':
                __is_publish = TRUE;
                break;
            case '?':
                __print_usage(argv[0]);
                return -1;
        }
            
    }

    if (argc - optind != 1)
    {
        __print_usage(argv[0]);
        return -1;
    }

    if (strcmp(argv[optind], "client") == 0)
    {
        // async call 
        GMainLoop* main_loop = g_main_loop_new(NULL, FALSE);

        printf("client mode.. with %d threads\n", thread_count);

        if (__is_publish)
        {
            pims_ipc_h ipc = NULL;
            ipc = pims_ipc_create_for_subscribe("pims-ipc-pub-test");
            if (!ipc)
            {
                printf("pims_ipc_create_for_subscribe error\n");
                return -1;
            }
            pims_ipc_destroy_for_subscribe(ipc);
            ipc = pims_ipc_create_for_subscribe("pims-ipc-pub-test");
            if (!ipc)
            {
                printf("pims_ipc_create_for_subscribe error\n");
                return -1;
            }
            if (pims_ipc_subscribe(ipc, "test_module", "publish", subscribe_callback, NULL) != 0) 
            {
                printf("pims_ipc_subscribe error\n");
                return -1;
            }
            if (pims_ipc_unsubscribe(ipc, "test_module", "publish") != 0) 
            {
                printf("pims_ipc_unsubscribe error\n");
                return -1;
            }
            if (pims_ipc_subscribe(ipc, "test_module", "publish", subscribe_callback, NULL) != 0) 
            {
                printf("pims_ipc_subscribe error\n");
                return -1;
            }
        }

        if (thread_count <= 1)
        {
            pims_ipc_h ipc = NULL;
            ipc = pims_ipc_create("pims-ipc-test");
            if (!ipc)
            {
                printf("pims_ipc_create error\n");
                return -1;
            }

            client_main(ipc, repeat_count, message_size);
        }
        else
        {
            arg.message_size = message_size; 
            arg.repeat_count = repeat_count; 
            arg.thread_count = thread_count; 
            if (__is_async || __is_publish)
                g_idle_add(__launch_worker, &arg);
            else
                __launch_worker(&arg);
        }

        if (__is_async || __is_publish)
            g_main_loop_run(main_loop);
    }
    else if (strcmp(argv[optind], "server") == 0)
    {
        printf("server mode..\n");
        server_main();
    }
    else
    {
        __print_usage(argv[0]);
        return -1;
    }
    return 0;
}
