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


#ifndef _NON_SLP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-daemon.h>

#include <pims-internal.h>
#include <pims-debug.h>
#include <pims-socket.h>

typedef struct
{
    int sockfd;
    server_socket_client_closed_cb callback;
    void *user_data;
    GHashTable *client_table;
} server_socket_context_t;

static int __socket_writen(int fd, char *buf, int buf_size)
{
    int ret, writed = 0;
    while (buf_size)
    {
        ret = write(fd, buf+writed, buf_size);
        if (-1 == ret)
        {
            if (EINTR == errno)
                continue;
            else
                return ret;
        }
        writed += ret;
        buf_size -= ret;
    }
    return writed;
}

static int __socket_readn(int fd, char *buf, int buf_size)
{
    int ret, read_size = 0;

    while (buf_size)
    {
        ret = read(fd, buf+read_size, buf_size);
        if (-1 == ret)
        {
            if (EINTR == errno)
                continue;
            else
                return ret;
        }
        read_size += ret;
        buf_size -= ret;
    }
    return read_size;
}

#define PIMS_IPC_PID_BUFFER_SIZE    20
static gboolean __request_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
    server_socket_context_t *context = (server_socket_context_t*)data;
    int ret = -1;
    int fd = -1;
    int orig_fd = -1;
    char *pid = NULL;
    char buffer[PIMS_IPC_PID_BUFFER_SIZE] = "";

    fd = g_io_channel_unix_get_fd(src);

    if (G_IO_HUP & condition)
    {
        close(fd);

        if (g_hash_table_lookup_extended(context->client_table, GINT_TO_POINTER(fd),
                    (gpointer*)&orig_fd, (gpointer*)&pid) == TRUE)
        {
            VERBOSE("found pid for %u = %s", fd, pid);
            context->callback((const char*)pid, context->user_data);
            g_hash_table_remove(context->client_table, (gconstpointer)fd);
        }
        else
        {
            VERBOSE("unable to find pid for %u", fd);
        }

        return FALSE;
    }

    memset(buffer, 0x00, PIMS_IPC_PID_BUFFER_SIZE);
    ret = read(fd, (char *)buffer, PIMS_IPC_PID_BUFFER_SIZE-1);
    if (ret <= 0)
    {
        ERROR("read error : %s", strerror(errno));
        close(fd);

        return FALSE;
    }

    VERBOSE("client fd = %u, pid = %s", fd, buffer);
    g_hash_table_insert(context->client_table, GINT_TO_POINTER(fd), g_strdup(buffer));

    pid_t mypid = getpid();
    ret = __socket_writen(fd, (char*)&mypid, sizeof(pid_t));
    if (ret != sizeof(pid_t))
    {
        ERROR("write error : %s", strerror(errno));
        close(fd);
        g_hash_table_remove(context->client_table, (gconstpointer)fd);

        return FALSE;
    }

   return TRUE;
}

static gboolean __socket_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
    GIOChannel *channel;
    server_socket_context_t *context = (server_socket_context_t*)data;
    int client_sockfd = -1;
    int sockfd = context->sockfd;
    struct sockaddr_un clientaddr;
    socklen_t client_len = sizeof(clientaddr);

    client_sockfd = accept(sockfd, (struct sockaddr *)&clientaddr, &client_len);
    if (-1 == client_sockfd)
    {
        ERROR("accept error : %s", strerror(errno));
        return TRUE;
    }

    channel = g_io_channel_unix_new(client_sockfd);
    g_io_add_watch(channel, G_IO_IN|G_IO_HUP, __request_handler, data);
    g_io_channel_unref(channel);

    return TRUE;
}

int _server_socket_init(const char *path, gid_t group, mode_t mode,
        server_socket_client_closed_cb callback, void *user_data)
{
    int sockfd = -1;
    GIOChannel *gio = NULL;

    if (sd_listen_fds(1) == 1 && sd_is_socket_unix(SD_LISTEN_FDS_START, SOCK_STREAM, -1, path, 0) > 0)
    {
        DEBUG("using system daemon");

        sockfd = SD_LISTEN_FDS_START;
    }
    else
    {
        struct sockaddr_un addr;
        int ret = -1;
        
        DEBUG("using local socket");

        unlink(path);

        bzero(&addr, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

        sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (-1 == sockfd)
        {
            ERROR("socket error : %s", strerror(errno));
            return -1;
        }

        ret = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
        if (-1 == ret)
        {
            ERROR("bind error : %s", strerror(errno));
            close(sockfd);
            return -1;
        }

        ret = chown(path, getuid(), group);
        ret = chmod(path, mode);

        ret = listen(sockfd, 30);
        if (-1 == ret)
        {
            ERROR("listen error : %s", strerror(errno));
            close(sockfd);
            return -1;
        }
    }

    gio = g_io_channel_unix_new(sockfd);

    server_socket_context_t *context = g_new0(server_socket_context_t, 1);
    context->sockfd = sockfd;
    context->callback = callback;
    context->user_data = user_data;
    context->client_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    ASSERT(context->client_table);

    g_io_add_watch(gio, G_IO_IN, __socket_handler, (gpointer)context);

    return sockfd;
}

int _client_socket_init(const char *path, const char *pid)
{
    int sockfd = -1;
    int ret = -1;
    struct sockaddr_un caddr = {0};
    pid_t server_pid = 0;

    ASSERT(path != NULL);
    ASSERT(pid != NULL);
    bzero(&caddr, sizeof(caddr));
    caddr.sun_family = AF_UNIX;
    snprintf(caddr.sun_path, sizeof(caddr.sun_path), "%s", path);

    sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (-1 == sockfd)
    {
        ERROR("socket error : %s", strerror(errno));
        return -1;
    }

    ret = connect(sockfd, (struct sockaddr *)&caddr, sizeof(caddr));
    if (-1 == ret) {
        ERROR("connect error : %s", strerror(errno));
        close(sockfd);
        return -1;
    }
    ret = __socket_writen(sockfd, (char*)pid, strlen(pid) + 1);
    if (ret <= 0)
    {
        ERROR("write error : %s", strerror(errno));
        close(sockfd);
        return -1;
    }
    ret = __socket_readn(sockfd, (char*)&server_pid, sizeof(pid_t));
    if (ret != sizeof(pid_t))
    {
        ERROR("read error : %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

#else
#include <pims-socket.h>

int _server_socket_init(const char *path, gid_t group, mode_t mode,
        server_socket_client_closed_cb callback, void *user_data)
{
    return 0;
}

int _client_socket_init(const char *path, const char *pid)
{
    return 0;
}

#endif
