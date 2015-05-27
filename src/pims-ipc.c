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

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <stdint.h>
#include <pthread.h>
#include <poll.h>			// pollfds
#include <sys/un.h>		// sockaddr_un
#include <sys/ioctl.h>		// ioctl
#include <sys/socket.h>		//socket
#include <sys/types.h>
#include <sys/epoll.h>		// epoll
#include <sys/eventfd.h>	// eventfd
#include <fcntl.h>
#include <errno.h>

#include "pims-internal.h"
#include "pims-socket.h"
#include "pims-debug.h"
#include "pims-ipc-data.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc.h"

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
	pims_ipc_subscribe_cb callback;
	void * user_data;
} pims_ipc_cb_s;

typedef struct
{
	char *call_id;
	pims_ipc_data_h *handle;
}pims_ipc_subscribe_data_s;

typedef struct
{
	int fd;
	char *service;
	char *id;
	GIOChannel *async_channel;
	guint async_source_id;
	pthread_mutex_t call_status_mutex;
	pims_ipc_call_status_e call_status;
	unsigned int call_sequence_no;
	pims_ipc_call_async_cb call_async_callback;
	void *call_async_userdata;
	pims_ipc_data_h dhandle_for_async_idler;

	int subscribe_fd;
	int epoll_stop_thread;
	pthread_t io_thread;
	GHashTable *subscribe_cb_table;

	pthread_mutex_t data_queue_mutex;
	GList *data_queue;
} pims_ipc_s;

static unsigned int ref_cnt;
static GList *subscribe_handles;

static void __sub_data_free(gpointer user_data)
{
	pims_ipc_subscribe_data_s *data = (pims_ipc_subscribe_data_s*)user_data;
	pims_ipc_data_destroy(data->handle);
	free(data->call_id);
	free(data);
}

static void __pims_ipc_free_handle(pims_ipc_s *handle)
{
	pthread_mutex_lock(&__gmutex);

	handle->epoll_stop_thread = true;

	if (handle->fd != -1)
		close(handle->fd);

	if (handle->io_thread)
		pthread_join(handle->io_thread, NULL);

	g_free(handle->id);
	g_free(handle->service);

	if (handle->async_channel) {
		// remove a subscriber handle from the golbal list
		subscribe_handles = g_list_remove(subscribe_handles, handle);
		VERBOSE("the count of subscribe handles = %d", g_list_length(subscribe_handles));

		g_source_remove(handle->async_source_id);
		g_io_channel_unref(handle->async_channel);
	}

	if (handle->subscribe_cb_table)
		g_hash_table_destroy(handle->subscribe_cb_table);

	pthread_mutex_lock(&handle->data_queue_mutex);
	if (handle->data_queue) {
		g_list_free_full(handle->data_queue, __sub_data_free);
	}
	pthread_mutex_unlock(&handle->data_queue_mutex);
	pthread_mutex_destroy(&handle->data_queue_mutex);

	if (handle->subscribe_fd != -1)
		close(handle->subscribe_fd);

	pthread_mutex_destroy(&handle->call_status_mutex);

	g_free(handle);

	if (--ref_cnt <= 0) {
		if (subscribe_handles)
			g_list_free(subscribe_handles);
		subscribe_handles = NULL;
	}

	pthread_mutex_unlock(&__gmutex);
}

static int __pims_ipc_receive_for_subscribe(pims_ipc_s *handle)
{
	pims_ipc_cb_s *cb_data = NULL;
	uint64_t dummy;

	do {
		read_command(handle->subscribe_fd, &dummy);

		pthread_mutex_lock(&handle->data_queue_mutex);
		if (!handle->data_queue) {
			pthread_mutex_unlock(&handle->data_queue_mutex);
			break;
		}

		GList *cursor = g_list_first(handle->data_queue);
		pims_ipc_subscribe_data_s *data = (pims_ipc_subscribe_data_s *)cursor->data;
		if (data == NULL) {
			pthread_mutex_unlock(&handle->data_queue_mutex);
			break;
		}

		cb_data = (pims_ipc_cb_s*)g_hash_table_lookup(handle->subscribe_cb_table, data->call_id);
		if (cb_data == NULL) {
			VERBOSE("unable to find %s", call_id);
		}
		else
			cb_data->callback((pims_ipc_h)handle, data->handle, cb_data->user_data);

		handle->data_queue = g_list_delete_link(handle->data_queue, cursor);
		__sub_data_free(data);
		pthread_mutex_unlock(&handle->data_queue_mutex);
	} while(1);

	return 0;
}

static gboolean __pims_ipc_subscribe_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	pims_ipc_s *handle = (pims_ipc_s *)data;

	VERBOSE("");

	if (condition & G_IO_HUP)
		return FALSE;

	pthread_mutex_lock(&__gmutex);

	// check if a subscriber handle is exists
	if (g_list_find(subscribe_handles, handle) == NULL) {
		ERROR("No such handle that ID is %p", handle);
		pthread_mutex_unlock(&__gmutex);
		return FALSE;
	}

	__pims_ipc_receive_for_subscribe(handle);

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

static int __pims_ipc_send_identify(pims_ipc_s *handle)
{
	unsigned int sequence_no;
	unsigned int client_id_len = strlen(handle->id);
	unsigned int len = sizeof(unsigned int)		// total size
		+ client_id_len + sizeof(unsigned int)		// client_id
		+ sizeof(unsigned int)	;	// seq_no

	char buf[len+1];
	int length = 0;
	memset(buf, 0x0, len+1);

	// total len
	memcpy(buf, (void*)&len, sizeof(unsigned int));
	length += sizeof(unsigned int);

	// client_id
	memcpy(buf+length, (void*)&(client_id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(handle->id), client_id_len);
	length += client_id_len;

	// seq_no
	GET_CALL_SEQUNECE_NO(handle, sequence_no);
	memcpy(buf+length, (void*)&(sequence_no), sizeof(unsigned int));
	length += sizeof(unsigned int);

	return socket_send(handle->fd, buf, length);
}

static int __pims_ipc_read_data(pims_ipc_s *handle, pims_ipc_data_h *data_out)
{
	int ret;
	gboolean is_ok = FALSE;
	int len = 0;
	pims_ipc_data_h data = NULL;
	unsigned int sequence_no = 0;
	char *client_id = NULL;
	char *call_id = NULL;
	char *buf = NULL;

	/* read the size of message. note that ioctl is non-blocking */
	if (ioctl(handle->fd, FIONREAD, &len)) {
		ERROR("ioctl failed: %d", errno);
		return -1;
	}

	/* when server or client closed socket */
	if (len == 0) {
		ERROR("[IPC Socket] connection is closed");
		return -1;
	}

	do {
		unsigned int read_len = 0;
		unsigned int total_len = 0;
		unsigned int client_id_len = 0;
		unsigned int call_id_len = 0;
		unsigned int is_data = FALSE;

		// get total_len
		read_len = TEMP_FAILURE_RETRY(read(handle->fd, (void *)&total_len, sizeof(unsigned int)));

		// client_id
		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(client_id_len), sizeof(unsigned int)));
		if (client_id_len > 0 && client_id_len < UINT_MAX-1) {
			client_id = calloc(1, client_id_len+1);
			if (client_id == NULL) {
				ERROR("calloc fail");
				break;
			}
		}
		else
			break;
		ret = socket_recv(handle->fd, (void *)&(client_id), client_id_len);
		if (ret < 0) {  ERROR("socket_recv error"); break;	}
		read_len += ret;

		// sequence no
		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(sequence_no), sizeof(unsigned int)));
		if (total_len == read_len) {
			// send identity
			data = pims_ipc_data_create(0);
			ret = pims_ipc_data_put(data, client_id, client_id_len);
			if (ret != 0)
				WARNING("pims_ipc_data_put fail(%d)", ret);
			break;
		}

		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(call_id_len), sizeof(unsigned int)));
		if (call_id_len > 0 && call_id_len < UINT_MAX-1) {
			call_id = calloc(1, call_id_len+1);
			if (call_id == NULL) {
				ERROR("calloc fail");
				break;
			}
		}
		else
			break;

		ret = socket_recv(handle->fd, (void *)&(call_id), call_id_len);
		if (ret < 0) {  ERROR("socket_recv error"); break;	}
		read_len += ret;

		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(is_data), sizeof(unsigned int)));
		if (is_data) {
			unsigned int data_len;
			read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(data_len), sizeof(unsigned int)));
			if (data_len > 0 && data_len < UINT_MAX-1) {
				buf = calloc(1, data_len+1);
				if (buf == NULL) {
					ERROR("calloc fail");
					break;
				}
			}
			else
				break;
			ret = socket_recv(handle->fd, (void *)&(buf), data_len);
			if (ret < 0) {  ERROR("socket_recv error"); break;	}
			read_len += ret;

			data = pims_ipc_data_steal_unmarshal(buf, data_len);
			buf = NULL;
		}

		INFO("client_id :%s, call_id : %s, seq_no : %d", client_id, call_id, sequence_no);
	} while(0);
	free(client_id);
	free(call_id);
	free(buf);

	if (sequence_no == handle->call_sequence_no) {
		if (data_out != NULL) {
			*data_out = data;
		}
		else if (data)
			pims_ipc_data_destroy(data);
		is_ok = TRUE;
	}
	else {
		if (data)
			pims_ipc_data_destroy(data);
		VERBOSE("received an mismatched response (%x:%x)", handle->call_sequence_no, sequence_no);
	}

	if (is_ok)
		return 0;

	return -1;
}

static int __pims_ipc_receive(pims_ipc_s *handle, pims_ipc_data_h *data_out)
{
	int ret = -1;
	struct pollfd *pollfds = (struct pollfd*) malloc (1 * sizeof (struct pollfd));

	pollfds[0].fd = handle->fd;
	pollfds[0].events = POLLIN | POLLERR | POLLHUP;

	while(1) {
		while(1) {
			ret = poll(pollfds, 1, 1000);
			if (ret == -1 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
				continue;
			}
			break;
		}

		if (ret > 0) {
			if (pollfds[0].revents & (POLLERR|POLLHUP)) {
				ERROR("Server disconnected");
				ret = -1;
				break;
			}
			if (pollfds[0].revents & POLLIN) {
				ret = __pims_ipc_read_data(handle, data_out);
				break;
			}
		}
	}
	free (pollfds);
	return ret;
}

static int __open_subscribe_fd(pims_ipc_s *handle)
{
	// router inproc eventfd
	int subscribe_fd = eventfd(0,0);
	int flags;
	int ret;

	if (-1 == subscribe_fd) {
		ERROR("eventfd error : %d", errno);
		return -1;
	}
	VERBOSE("subscribe :%d\n", subscribe_fd);

	flags = fcntl (subscribe_fd, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	ret = fcntl (subscribe_fd, F_SETFL, flags | O_NONBLOCK);
	VERBOSE("subscribe fcntl : %d\n", ret);

	handle->subscribe_fd = subscribe_fd;
	return 0;
}

static int __subscribe_data(pims_ipc_s * handle)
{
	int len;
	int ret = -1;
	char *call_id = NULL;
	char *buf = NULL;
	pims_ipc_data_h dhandle = NULL;

	do {
		/* read the size of message. note that ioctl is non-blocking */
		if (ioctl(handle->fd, FIONREAD, &len)) {
			ERROR("ioctl failed: %d", errno);
			break;
		}

		/* when server or client closed socket */
		if (len == 0) {
			INFO("[IPC Socket] connection is closed");
			break;
		}

		unsigned int read_len = 0;
		unsigned int total_len = 0;
		unsigned int call_id_len = 0;
		unsigned int is_data = FALSE;

		// get total_len
		read_len = TEMP_FAILURE_RETRY(read(handle->fd, (void *)&total_len, sizeof(unsigned int)));

		// call_id
		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(call_id_len), sizeof(unsigned int)));
		if (call_id_len > 0 && call_id_len < UINT_MAX-1) {
			call_id = calloc(1, call_id_len+1);
			if (call_id == NULL) {
				ERROR("calloc fail");
				break;
			}
		}
		else
			break;

		ret = socket_recv(handle->fd, (void *)&(call_id), call_id_len);
		if (ret < 0) {  ERROR("socket_recv error"); break; }
		read_len += ret;

		// is_data
		read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(is_data), sizeof(unsigned int)));

		if (is_data) {
			unsigned int data_len;
			read_len += TEMP_FAILURE_RETRY(read(handle->fd, (void *)&(data_len), sizeof(unsigned int)));
			if (data_len > 0 && data_len < UINT_MAX-1) {
				buf = calloc(1, data_len+1);
				if (buf == NULL) {
					ERROR("calloc fail");
					break;
				}
			}
			else
				break;
			ret = socket_recv(handle->fd, (void *)&(buf), data_len);
			if (ret < 0) {  ERROR("socket_recv error"); break; }
			read_len += ret;

			dhandle = pims_ipc_data_steal_unmarshal(buf, data_len);
			buf = NULL;

			pims_ipc_subscribe_data_s *sub_data = (pims_ipc_subscribe_data_s *)calloc(1, sizeof(pims_ipc_subscribe_data_s));
			sub_data->handle = dhandle;
			sub_data->call_id = call_id;
			call_id = NULL;

			pthread_mutex_lock(&handle->data_queue_mutex);
			handle->data_queue = g_list_append(handle->data_queue, sub_data);
			pthread_mutex_unlock(&handle->data_queue_mutex);
			write_command(handle->subscribe_fd, 1);
		}
		ret = 0;
	}while(0);

	free(call_id);
	free(buf);
	return ret;
}

static void* __io_thread(void *data)
{
	pims_ipc_s *handle = data;
	struct epoll_event ev = {0};
	int ret;
	int epfd;

	epfd = epoll_create(MAX_EPOLL_EVENT);

	ev.events = EPOLLIN | EPOLLHUP;
	ev.data.fd = handle->fd;

	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, handle->fd, &ev);
	WARN_IF(ret != 0, "listen error :%d", ret);

	while (!handle->epoll_stop_thread) {
		int i = 0;
		struct epoll_event events[MAX_EPOLL_EVENT] = {{0}, };
		int event_num = epoll_wait(epfd, events, MAX_EPOLL_EVENT, 50);

		if (handle->epoll_stop_thread)
			break;

		if (event_num == -1) {
			if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				ERROR("errno:%d\n", errno);
				break;
			}
		}

		for (i = 0; i < event_num; i++) {
			if (events[i].events & EPOLLHUP) {
				ERROR("server fd closed");
				handle->epoll_stop_thread = true;
				break;
			}

			if (events[i].events & EPOLLIN) {
				if(__subscribe_data(handle) < 0)
					break;
			}
		}
	}

	close(epfd);

	pthread_exit(NULL);
}

static pims_ipc_h __pims_ipc_create(char *service, pims_ipc_mode_e mode)
{
	pims_ipc_s *handle = NULL;
	gboolean is_ok = FALSE;

	pthread_mutex_lock(&__gmutex);

	do {
		struct sockaddr_un server_addr;
		int ret;

		ref_cnt++;
		VERBOSE("Create %d th..", ref_cnt);

		handle = g_new0(pims_ipc_s, 1);
		if (handle == NULL) {
			ERROR("Failed to allocation");
			break;
		}

		handle->subscribe_fd = -1;
		handle->io_thread = 0;
		handle->service = g_strdup(service);
		handle->id = g_strdup_printf("%x:%x", getpid(), __get_global_sequence_no());
		handle->fd = socket(PF_UNIX, SOCK_STREAM, 0);
		if (handle->fd < 0) {
			ERROR("socket error : %d, errno: %d", handle->fd, errno);
			break;
		}
		int flags = fcntl (handle->fd, F_GETFL, 0);
		if (flags == -1)
			flags = 0;
		ret = fcntl (handle->fd, F_SETFL, flags | O_NONBLOCK);
		VERBOSE("socket fcntl : %d\n", ret);

		pthread_mutex_init(&handle->call_status_mutex, 0);

		pthread_mutex_lock(&handle->call_status_mutex);
		handle->call_status = PIMS_IPC_CALL_STATUS_READY;
		pthread_mutex_unlock(&handle->call_status_mutex);

		bzero(&server_addr, sizeof(server_addr));
		server_addr.sun_family = AF_UNIX;
		snprintf(server_addr.sun_path, sizeof(server_addr.sun_path), "%s", handle->service);

		ret = connect(handle->fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
		if (ret != 0) {
			ERROR("connect error : %d, errno: %d", ret, errno);
			break;
		}
		VERBOSE("connect to server : socket:%s, client_sock:%d, %d\n", handle->service, handle->fd, ret);

		if (mode == PIMS_IPC_MODE_REQ) {
			handle->call_sequence_no = (unsigned int)time(NULL);
			ret = __pims_ipc_send_identify(handle);
			if (ret < 0) {
				ERROR("__pims_ipc_send_identify error");
				break;
			}
			__pims_ipc_receive(handle, NULL);

			if (pims_ipc_call(handle, PIMS_IPC_MODULE_INTERNAL, PIMS_IPC_FUNCTION_CREATE, NULL, NULL) != 0) {
				WARNING("pims_ipc_call(PIMS_IPC_FUNCTION_CREATE) failed");
			}
		}
		else {
			handle->epoll_stop_thread = false;
			pthread_mutex_init(&handle->data_queue_mutex, 0);

			pthread_mutex_lock(&handle->data_queue_mutex);
			handle->data_queue = NULL;
			pthread_mutex_unlock(&handle->data_queue_mutex);

			ret = __open_subscribe_fd(handle);
			if (ret < 0)
				break;

			pthread_t worker;
			ret = pthread_create(&worker, NULL, __io_thread, handle);
			if (ret != 0)
				break;
			handle->io_thread  = worker;

			GIOChannel *async_channel = g_io_channel_unix_new(handle->subscribe_fd);
			if (!async_channel) {
				ERROR("g_io_channel_unix_new error");
				break;
			}
			handle->async_channel = async_channel;
			handle->async_source_id = g_io_add_watch(handle->async_channel, G_IO_IN|G_IO_HUP, __pims_ipc_subscribe_handler, handle);
			handle->subscribe_cb_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			ASSERT(handle->subscribe_cb_table);

			// add a subscriber handle to the global list
			subscribe_handles = g_list_append(subscribe_handles, handle);
			VERBOSE("the count of subscribe handles = %d", g_list_length(subscribe_handles));
		}

		is_ok = TRUE;
		VERBOSE("A new handle is created : %s, %s", handle->service, handle->id);
	} while(0);

	pthread_mutex_unlock(&__gmutex);

	if (FALSE == is_ok) {
		if (handle) {
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
	pims_ipc_s *handle = (pims_ipc_s *)ipc;

	if (mode == PIMS_IPC_MODE_REQ) {
		if (pims_ipc_call(handle, PIMS_IPC_MODULE_INTERNAL, PIMS_IPC_FUNCTION_DESTROY, NULL, NULL) != 0) {
			WARNING("pims_ipc_call(PIMS_IPC_FUNCTION_DESTROY) failed");
		}
	}

	if (handle)
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

static int __pims_ipc_send(pims_ipc_s *handle, char *module, char *function, pims_ipc_data_h data_in)
{
	int ret = -1;
	unsigned int sequence_no = 0;
	gchar *call_id = PIMS_IPC_MAKE_CALL_ID(module, function);
	unsigned int call_id_len = strlen(call_id);
	pims_ipc_data_s *data = NULL;
	unsigned int is_data = FALSE;
	unsigned int client_id_len = strlen(handle->id);
	int length = 0;

	GET_CALL_SEQUNECE_NO(handle, sequence_no);

	int len = sizeof(unsigned int)						// total size
		+ client_id_len + sizeof(unsigned int)	// client_id
		+ sizeof(unsigned int)						// seq_no
		+ call_id_len + sizeof(unsigned int)	// call_id
		+ sizeof(unsigned int);						// is data

	int total_len = len;

	if (data_in) {
		is_data = TRUE;
		data = (pims_ipc_data_s*)data_in;
		len += sizeof(unsigned int);
		total_len = len + data->buf_size;
	}

	INFO("len : %d, client_id : %s, call_id : %s, seq_no :%d", len, handle->id, call_id, sequence_no);

	char buf[len+1];

	memset(buf, 0x0, len+1);

	memcpy(buf, (void*)&total_len, sizeof(unsigned int));
	length += sizeof(unsigned int);

	// client_id
	client_id_len = strlen(handle->id);
	memcpy(buf+length, (void*)&(client_id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(handle->id), client_id_len);
	length += client_id_len;

	// seq_no
	memcpy(buf+length, (void*)&(sequence_no), sizeof(unsigned int));
	length += sizeof(unsigned int);

	// call id
	memcpy(buf+length, (void*)&(call_id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(call_id), call_id_len);
	length += call_id_len;
	g_free(call_id);

	// is_data
	memcpy(buf+length, (void*)&(is_data), sizeof(unsigned int));
	length += sizeof(unsigned int);

	if (is_data) {
		memcpy(buf+length, (void*)&(data->buf_size), sizeof(unsigned int));
		length += sizeof(unsigned int);

		ret = socket_send(handle->fd, buf, length);
		if (ret > 0)
			ret = socket_send_data(handle->fd, data->buf, data->buf_size);
	}
	else {
		ret = socket_send(handle->fd, buf, length);
	}

	if (ret < 0)
		return -1;

	return 0;
}

API int pims_ipc_call(pims_ipc_h ipc, char *module, char *function, pims_ipc_data_h data_in,
		pims_ipc_data_h *data_out)
{
	pims_ipc_s *handle = (pims_ipc_s *)ipc;


	if (ipc == NULL) {
		ERROR("invalid handle : %p", ipc);
		return -1;
	}

	if (!module || !function) {
		ERROR("invalid argument");
		return -1;
	}

	pthread_mutex_lock(&handle->call_status_mutex);
	if (handle->call_status != PIMS_IPC_CALL_STATUS_READY) {
		pthread_mutex_unlock(&handle->call_status_mutex);
		ERROR("the previous call is in progress : %p", ipc);
		return -1;
	}
	pthread_mutex_unlock(&handle->call_status_mutex);


	if (__pims_ipc_send(handle, module, function, data_in) != 0) {
		return -1;
	}

	if (__pims_ipc_receive(handle, data_out) != 0) {
		return -1;
	}

	return 0;
}

static gboolean __call_async_idler_cb(gpointer data)
{
	VERBOSE("");

	pims_ipc_s *handle = (pims_ipc_s *)data;
	ASSERT(handle);
	ASSERT(handle->dhandle_for_async_idler);
	pims_ipc_data_h dhandle = handle->dhandle_for_async_idler;
	handle->dhandle_for_async_idler = NULL;

	pthread_mutex_lock(&handle->call_status_mutex);
	handle->call_status = PIMS_IPC_CALL_STATUS_READY;
	pthread_mutex_unlock(&handle->call_status_mutex);

	handle->call_async_callback((pims_ipc_h)handle, dhandle, handle->call_async_userdata);
	pims_ipc_data_destroy(dhandle);

	return FALSE;
}

static gboolean __pims_ipc_call_async_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	pims_ipc_s *handle = (pims_ipc_s *)data;
	pims_ipc_data_h dhandle = NULL;

	if (__pims_ipc_receive(handle, &dhandle) == 0) {
		VERBOSE("call status = %d", handle->call_status);

		pthread_mutex_lock(&handle->call_status_mutex);
		if (handle->call_status != PIMS_IPC_CALL_STATUS_IN_PROGRESS) {
			pthread_mutex_unlock(&handle->call_status_mutex);
			pims_ipc_data_destroy(dhandle);
		}
		else {
			pthread_mutex_unlock(&handle->call_status_mutex);
			if (src == NULL) {    // A response is arrived too quickly
				handle->dhandle_for_async_idler = dhandle;
				g_idle_add(__call_async_idler_cb, handle);
			}
			else {
				pthread_mutex_lock(&handle->call_status_mutex);
				handle->call_status = PIMS_IPC_CALL_STATUS_READY;
				pthread_mutex_unlock(&handle->call_status_mutex);

				handle->call_async_callback((pims_ipc_h)handle, dhandle, handle->call_async_userdata);
				pims_ipc_data_destroy(dhandle);
			}
		}
	}
	return FALSE;
}

API int pims_ipc_call_async(pims_ipc_h ipc, char *module, char *function, pims_ipc_data_h data_in,
		pims_ipc_call_async_cb callback, void *userdata)
{
	pims_ipc_s *handle = (pims_ipc_s *)ipc;
	guint source_id = 0;

	if (ipc == NULL) {
		ERROR("invalid handle : %p", ipc);
		return -1;
	}

	if (!module || !function || !callback) {
		ERROR("invalid argument");
		return -1;
	}

	pthread_mutex_lock(&handle->call_status_mutex);
	if (handle->call_status != PIMS_IPC_CALL_STATUS_READY) {
		pthread_mutex_unlock(&handle->call_status_mutex);
		ERROR("the previous call is in progress : %p", ipc);
		return -1;
	}
	pthread_mutex_unlock(&handle->call_status_mutex);

	pthread_mutex_lock(&handle->call_status_mutex);
	handle->call_status = PIMS_IPC_CALL_STATUS_IN_PROGRESS;
	pthread_mutex_unlock(&handle->call_status_mutex);

	handle->call_async_callback = callback;
	handle->call_async_userdata = userdata;

	// add a callback for GIOChannel
	if (!handle->async_channel) {
		handle->async_channel = g_io_channel_unix_new(handle->fd);
		if (!handle->async_channel) {
			ERROR("g_io_channel_unix_new error");
			return -1;
		}
	}

	source_id = g_io_add_watch(handle->async_channel, G_IO_IN, __pims_ipc_call_async_handler, handle);
	handle->async_source_id = source_id;

	if (__pims_ipc_send(handle, module, function, data_in) != 0) {
		g_source_remove(source_id);
		return -1;
	}

	__pims_ipc_call_async_handler(NULL, G_IO_NVAL, handle);

	return 0;
}

API bool pims_ipc_is_call_in_progress(pims_ipc_h ipc)
{
	int ret;
	pims_ipc_s *handle = (pims_ipc_s *)ipc;

	if (ipc == NULL) {
		ERROR("invalid handle : %p", ipc);
		return false;
	}

	pthread_mutex_lock(&handle->call_status_mutex);
	if (handle->call_status == PIMS_IPC_CALL_STATUS_IN_PROGRESS)
		ret = true;
	else
		ret = false;
	pthread_mutex_unlock(&handle->call_status_mutex);
	return ret;
}

API int pims_ipc_subscribe(pims_ipc_h ipc, char *module, char *event, pims_ipc_subscribe_cb callback, void *userdata)
{
	gchar *call_id = NULL;
	pims_ipc_cb_s *cb_data = NULL;
	pims_ipc_s *handle = (pims_ipc_s *)ipc;

	if (ipc == NULL || handle->subscribe_cb_table == NULL) {
		ERROR("invalid handle : %p", ipc);
		return -1;
	}

	if (!module || !event || !callback) {
		ERROR("invalid argument");
		return -1;
	}

	cb_data = g_new0(pims_ipc_cb_s, 1);
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
	pims_ipc_s *handle = (pims_ipc_s *)ipc;

	if (ipc == NULL || handle->subscribe_cb_table == NULL) {
		ERROR("invalid handle : %p", ipc);
		return -1;
	}

	if (!module || !event) {
		ERROR("invalid argument");
		return -1;
	}

	call_id = PIMS_IPC_MAKE_CALL_ID(module, event);

	VERBOSE("unsubscribe cb id[%s]", call_id);

	if (g_hash_table_remove(handle->subscribe_cb_table, call_id) != TRUE) {
		ERROR("g_hash_table_remove error");
		g_free(call_id);
		return -1;
	}

	g_free(call_id);
	return 0;
}

