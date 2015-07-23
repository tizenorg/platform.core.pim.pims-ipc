/*
 * PIMS IPC
 *
 * Copyright (c) 2012 - 2015 Samsung Electronics Co., Ltd. All rights reserved.
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
#include <poll.h>				// pollfds
#include <fcntl.h>				//fcntl
#include <unistd.h>
#include <systemd/sd-daemon.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/un.h>			// sockaddr_un
#include <sys/ioctl.h>		// ioctl
#include <sys/epoll.h>		// epoll
#include <sys/eventfd.h>	// eventfd
#include <sys/socket.h>		//socket
#include <sys/types.h>

#include <cynara-client.h>
#include <cynara-session.h>
#include <cynara-creds-socket.h>

#include "pims-internal.h"
#include "pims-debug.h"
#include "pims-socket.h"
#include "pims-ipc-data.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc-svc.h"

#define PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT  2

typedef struct {
	char *service;
	gid_t group;
	mode_t mode;

	// callback functions
	GHashTable *cb_table;			// call_id, cb_data

	// Global socket info and epoll thread
	int sockfd;
	bool epoll_stop_thread;

	/////////////////////////////////////////////
	// router inproc eventfd
	int router;
	int delay_count;  // not need mutex
	// epoll thread add client_fd, when receive, router read requests
	GList *request_queue;	 // client_id lists to send request
	pthread_mutex_t request_data_queue_mutex;
	GHashTable *request_data_queue;  // key : client id, data : GList pims_ipc_raw_data_s (client_fd, seq_no, request(command), additional data...)
	// router add client when receive connecting request, remove client when disconneting request in router thread
	// manager remove client when terminating client without disconnect request in router thread
	GHashTable *client_worker_map;		 // key : client_id, worker_fd, not need mutex
	GList *client_id_fd_map;		 // pims_ipc_client_map_s
	//key :client_id(pid:seq_no), data : client_fd

	/////////////////////////////////////////////
	pthread_mutex_t task_fds_mutex;
	// when starting worker thread, register fd
	// when endting worker thread, deregister fd
	GHashTable *task_fds;	 // worker_fd - worker data (worker fd, client_fd, request queue(GList), stop_thread)
	int workers_max_count;

	/////////////////////////////////////////////
	// manager inproc eventfd
	int manager;
	// write by new worker thread, read by manager in router thread, need mutex
	pthread_mutex_t manager_queue_from_worker_mutex;
	GList *manager_queue_from_worker;	 // worker_fd => add to workers
	// write in epoll thread(for dead client), read by manager in router thread, need mutex
	pthread_mutex_t manager_queue_from_epoll_mutex;
	GList *manager_queue_from_epoll; // cliend_fd => find worker_fd => add to idle workers
	// managed by manager, router find idle worker when connecting new client in router thread => remove from idle workers
	GList *workers;		 // worker_fd list, not need mutex
	/////////////////////////////////////////////
	cynara *cynara;
	pthread_mutex_t cynara_mutex;

	int unique_sequence_number;
	pthread_mutex_t client_info_mutex;
	GHashTable *worker_client_info_map; // key : worker_id, data : pims_ipc_client_info_s*
	GHashTable *client_info_map; // key : client_id, data : pims_ipc_client_info_s*
} pims_ipc_svc_s;

typedef struct {
	char *smack;
	char *uid;
	char *client_session;
} pims_ipc_client_info_s ;

typedef struct {
	char *service;
	gid_t group;
	mode_t mode;

	int publish_sockfd;
	bool epoll_stop_thread;
	pthread_mutex_t subscribe_fds_mutex;
	GList *subscribe_fds;		// cliend fd list
} pims_ipc_svc_for_publish_s;

typedef struct {
	int fd;
	char *id;
}pims_ipc_client_map_s;

typedef struct {
	pims_ipc_svc_call_cb callback;
	void * user_data;
} pims_ipc_svc_cb_s;

typedef struct {
	pims_ipc_svc_client_disconnected_cb callback;
	void * user_data;
} pims_ipc_svc_client_disconnected_cb_t;

typedef struct {
	int fd;
	int worker_id;	// pthrad_self()
	int client_fd;
	bool stop_thread;
	GList *queue;		// pims_ipc_raw_data_s list
	pthread_mutex_t queue_mutex;
} pims_ipc_worker_data_s;

typedef struct{
	char *client_id;
	unsigned int client_id_len;
	unsigned int seq_no;
	char *call_id;
	unsigned int call_id_len;
	unsigned int is_data;
	unsigned int data_len;
	char *data;
}pims_ipc_raw_data_s;

typedef struct {
	int client_fd;
	int request_count;
	GList *raw_data;		// pims_ipc_raw_data_s list
	pthread_mutex_t raw_data_mutex;
}pims_ipc_request_s;

static pims_ipc_svc_s *_g_singleton = NULL;
static pims_ipc_svc_for_publish_s *_g_singleton_for_publish = NULL;

static __thread pims_ipc_svc_client_disconnected_cb_t _client_disconnected_cb = {NULL, NULL};

static void __free_raw_data(pims_ipc_raw_data_s *data)
{
	if (!data) return;

	free(data->client_id);
	free(data->call_id);
	free(data->data);
	free(data);
}

static void __worker_data_free(gpointer data)
{
	pims_ipc_worker_data_s *worker_data = (pims_ipc_worker_data_s*)data;

	pthread_mutex_lock(&worker_data->queue_mutex);
	if (worker_data->queue) {
		GList *cursor = g_list_first(worker_data->queue);
		while(cursor) {
			GList *l = cursor;
			pims_ipc_raw_data_s *data = l->data;
			cursor = g_list_next(cursor);
			worker_data->queue = g_list_remove_link(worker_data->queue, l);
			g_list_free(l);
			__free_raw_data(data);
		}
	}
	pthread_mutex_unlock(&worker_data->queue_mutex);
	free(worker_data);
}

static void _destroy_client_info(gpointer p)
{
	pims_ipc_client_info_s *client_info = p;

	if (NULL == client_info)
		return;
	free(client_info->smack);
	free(client_info->uid);
	free(client_info->client_session);
	free(client_info);
}

API int pims_ipc_svc_init(char *service, gid_t group, mode_t mode)
{
	if (_g_singleton) {
		ERROR("Already exist");
		return -1;
	}

	_g_singleton = g_new0(pims_ipc_svc_s, 1);
	ASSERT(_g_singleton);

	_g_singleton->service = g_strdup(service);
	_g_singleton->group = group;
	_g_singleton->mode = mode;
	_g_singleton->workers_max_count = PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT;
	_g_singleton->cb_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	ASSERT(_g_singleton->cb_table);

	pthread_mutex_init(&_g_singleton->request_data_queue_mutex, 0);
	_g_singleton->request_queue = NULL;
	_g_singleton->request_data_queue = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);	// client_id - pims_ipc_raw_data_s
	ASSERT(_g_singleton->request_data_queue);
	_g_singleton->client_worker_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);		// client id - worker_fd mapping
	ASSERT(_g_singleton->client_worker_map);
	_g_singleton->delay_count = 0;

	pthread_mutex_init(&_g_singleton->task_fds_mutex, 0);
	_g_singleton->task_fds = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, __worker_data_free);		// pims_ipc_worker_data_s
	ASSERT(_g_singleton->task_fds);

	pthread_mutex_init(&_g_singleton->manager_queue_from_epoll_mutex, 0);
	_g_singleton->manager_queue_from_epoll = NULL;

	pthread_mutex_init(&_g_singleton->manager_queue_from_worker_mutex, 0);
	_g_singleton->manager_queue_from_worker = NULL;
	_g_singleton->workers = NULL;

	_g_singleton->unique_sequence_number = 0;

	_g_singleton->worker_client_info_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _destroy_client_info);
	_g_singleton->client_info_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _destroy_client_info);
	pthread_mutex_init(&_g_singleton->client_info_mutex, 0);

	pthread_mutex_init(&_g_singleton->cynara_mutex, 0);
	_g_singleton->epoll_stop_thread = false;

	int ret = cynara_initialize(&_g_singleton->cynara, NULL);
	if (CYNARA_API_SUCCESS != ret) {
		char errmsg[1024] = {0};
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERROR("cynara_initialize() Fail(%d,%s)", ret, errmsg);
		return -1;
	}
	return 0;
}

API int pims_ipc_svc_deinit(void)
{
	if (!_g_singleton)
		return -1;

	g_free(_g_singleton->service);
	g_hash_table_destroy(_g_singleton->cb_table);

	pthread_mutex_destroy(&_g_singleton->request_data_queue_mutex);
	g_hash_table_destroy(_g_singleton->client_worker_map);
	g_hash_table_destroy(_g_singleton->request_data_queue);
	g_list_free_full(_g_singleton->request_queue, g_free);

	pthread_mutex_destroy(&_g_singleton->task_fds_mutex);
	g_hash_table_destroy(_g_singleton->task_fds);

	pthread_mutex_destroy(&_g_singleton->manager_queue_from_epoll_mutex);
	g_list_free_full(_g_singleton->manager_queue_from_epoll, g_free);
	pthread_mutex_destroy(&_g_singleton->manager_queue_from_worker_mutex);
	g_list_free(_g_singleton->manager_queue_from_worker);

	GList *cursor = g_list_first(_g_singleton->client_id_fd_map);
	while(cursor) {
		pims_ipc_client_map_s *client = cursor->data;
		_g_singleton->client_id_fd_map = g_list_remove_link(_g_singleton->client_id_fd_map, cursor);			//free(client_id);
		free(client->id);
		free(client);
		g_list_free(cursor);
		cursor = g_list_first(_g_singleton->client_id_fd_map);
	}
	g_list_free(_g_singleton->client_id_fd_map);

	pthread_mutex_destroy(&_g_singleton->client_info_mutex);
	g_hash_table_destroy(_g_singleton->worker_client_info_map);
	g_hash_table_destroy(_g_singleton->client_info_map);

	pthread_mutex_lock(&_g_singleton->cynara_mutex);
	int ret = cynara_finish(_g_singleton->cynara);
	if (CYNARA_API_SUCCESS != ret) {
		char errmsg[1024] = {0};
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERROR("cynara_finish() Fail(%d,%s)", ret, errmsg);
	}
	pthread_mutex_unlock(&_g_singleton->cynara_mutex);
	pthread_mutex_destroy(&_g_singleton->cynara_mutex);

	g_list_free(_g_singleton->workers);
	g_free(_g_singleton);
	_g_singleton = NULL;

	return 0;
}

API int pims_ipc_svc_register(char *module, char *function, pims_ipc_svc_call_cb callback, void *userdata)
{
	pims_ipc_svc_cb_s *cb_data = NULL;
	gchar *call_id = NULL;

	if (!module || !function || !callback) {
		ERROR("Invalid argument");
		return -1;
	}
	cb_data = g_new0(pims_ipc_svc_cb_s, 1);
	call_id = PIMS_IPC_MAKE_CALL_ID(module, function);

	VERBOSE("register cb id[%s]", call_id);
	cb_data->callback = callback;
	cb_data->user_data = userdata;
	g_hash_table_insert(_g_singleton->cb_table, call_id, cb_data);

	return 0;
}

API int pims_ipc_svc_init_for_publish(char *service, gid_t group, mode_t mode)
{
	if (_g_singleton_for_publish) {
		ERROR("Already exist");
		return -1;
	}

	_g_singleton_for_publish = g_new0(pims_ipc_svc_for_publish_s, 1);
	_g_singleton_for_publish->service = g_strdup(service);
	_g_singleton_for_publish->group = group;
	_g_singleton_for_publish->mode = mode;
	_g_singleton_for_publish->subscribe_fds = NULL;

	pthread_mutex_init(&_g_singleton_for_publish->subscribe_fds_mutex, 0);

	return 0;
}

API int pims_ipc_svc_deinit_for_publish(void)
{
	if (!_g_singleton_for_publish)
		return -1;

	pthread_mutex_destroy(&_g_singleton_for_publish->subscribe_fds_mutex);
	g_list_free(_g_singleton_for_publish->subscribe_fds);

	g_free(_g_singleton_for_publish->service);
	g_free(_g_singleton_for_publish);
	_g_singleton_for_publish = NULL;

	return 0;
}

API int pims_ipc_svc_publish(char *module, char *event, pims_ipc_data_h data)
{
	pims_ipc_svc_for_publish_s *ipc_svc = _g_singleton_for_publish;
	gboolean is_valid = FALSE;
	gchar *call_id = PIMS_IPC_MAKE_CALL_ID(module, event);
	pims_ipc_data_s *data_in = (pims_ipc_data_s*)data;
	unsigned int call_id_len = strlen(call_id);
	unsigned int is_data = FALSE;

	do {
		// make publish data
		unsigned int len = sizeof(unsigned int)						// total size
			+ call_id_len + sizeof(unsigned int)			// call_id
			+ sizeof(unsigned int);							// is data
		unsigned int total_len = len;

		if (data_in) {
			is_data = TRUE;
			len += sizeof(unsigned int);
			total_len = len + data_in->buf_size;			// data
		}

		char buf[len+1];
		int length = 0;
		memset(buf, 0x0, len+1);

		// total_size
		memcpy(buf, (void*)&total_len, sizeof(unsigned int));
		length += sizeof(unsigned int);

		// call_id
		memcpy(buf+length, (void*)&(call_id_len), sizeof(unsigned int));
		length += sizeof(unsigned int);
		memcpy(buf+length, (void*)(call_id), call_id_len);
		length += call_id_len;
		g_free(call_id);

		// is_data
		memcpy(buf+length, (void*)&(is_data), sizeof(unsigned int));
		length += sizeof(unsigned int);

		// data
		if (is_data) {
			memcpy(buf+length, (void*)&(data_in->buf_size), sizeof(unsigned int));
			length += sizeof(unsigned int);
		}

		// Publish to clients
		pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
		GList *cursor = g_list_first(ipc_svc->subscribe_fds);
		int ret = 0;
		while(cursor) {
			int fd = (int)cursor->data;
			ret = socket_send(fd, buf, length);
			if (ret < 0) {
				ERROR("socket_send publish error : %d", ret);
			}

			if (is_data) {
				ret = socket_send_data(fd, data_in->buf, data_in->buf_size);
				if (ret < 0) {
					ERROR("socket_send_data publish error : %d", ret);
				}
			}
			cursor = g_list_next(cursor);
		}
		pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);

		is_valid = TRUE;
	} while (0);

	if (is_valid == FALSE)
		return -1;
	return 0;
}

static void __run_callback(int worker_id, char *call_id, pims_ipc_data_h dhandle_in, pims_ipc_data_h *dhandle_out)
{
	pims_ipc_svc_cb_s *cb_data = NULL;

	VERBOSE("Call id [%s]", call_id);

	cb_data = (pims_ipc_svc_cb_s*)g_hash_table_lookup(_g_singleton->cb_table, call_id);
	if (cb_data == NULL) {
		VERBOSE("unable to find %s", call_id);
		return;
	}

	cb_data->callback((pims_ipc_h)worker_id, dhandle_in, dhandle_out, cb_data->user_data);
}

static void __make_raw_data(const char *call_id, int seq_no, pims_ipc_data_h data, pims_ipc_raw_data_s **out)
{
	if (NULL == out) {
		ERROR("Invalid parameter:out is NULL");
		return;
	}

	pims_ipc_raw_data_s *raw_data = NULL;
	raw_data = (pims_ipc_raw_data_s*)calloc(1, sizeof(pims_ipc_raw_data_s));
	if (NULL == raw_data) {
		ERROR("calloc() Fail");
		return;
	}
	pims_ipc_data_s *data_in = (pims_ipc_data_s*)data;

	raw_data->call_id = strdup(call_id);
	raw_data->call_id_len = strlen(raw_data->call_id);
	raw_data->seq_no = seq_no;

	if (data_in && data_in->buf_size > 0) {
		raw_data->is_data = TRUE;
		raw_data->data = calloc(1, data_in->buf_size+1);
		if (NULL == raw_data->data) {
			ERROR("calloc() Fail");
			free(raw_data->call_id);
			free(raw_data);
			return;
		}
		memcpy(raw_data->data, data_in->buf, data_in->buf_size);
		raw_data->data_len = data_in->buf_size;
	}
	else {
		raw_data->is_data = FALSE;
		raw_data->data_len = 0;
		raw_data->data = NULL;
	}
	*out = raw_data;
	return;
}

static int __send_raw_data(int fd, const char *client_id, pims_ipc_raw_data_s *data)
{
	int ret = 0;
	unsigned int client_id_len = strlen(client_id);

	if (!data) {
		INFO("No data to send NULL\n");
		return -1;
	}

	unsigned int len = sizeof(unsigned int)			// total size
		+ client_id_len + sizeof(unsigned int)		// client_id
		+ sizeof(unsigned int)							// seq_no
		+ data->call_id_len + sizeof(unsigned int)	// call_id
		+ sizeof(unsigned int);							// is data
	unsigned int total_len = len;

	if (data->is_data) {
		len += sizeof(unsigned int); // data
		total_len = len + data->data_len;		// data
	}

	INFO("client_id: %s, call_id : %s, seq no :%d, len:%d, total len :%d", client_id, data->call_id, data->seq_no, len, total_len);

	char buf[len+1];

	int length = 0;
	memset(buf, 0x0, len+1);

	// total_len
	memcpy(buf, (void*)&total_len, sizeof(unsigned int));
	length += sizeof(unsigned int);

	// client_id
	memcpy(buf+length, (void*)&(client_id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(client_id), client_id_len);
	length += client_id_len;

	// seq_no
	memcpy(buf+length, (void*)&(data->seq_no), sizeof(unsigned int));
	length += sizeof(unsigned int);

	// call id
	memcpy(buf+length, (void*)&(data->call_id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(data->call_id), data->call_id_len);
	length += data->call_id_len;

	// is_data
	memcpy(buf+length, (void*)&(data->is_data), sizeof(unsigned int));
	length += sizeof(unsigned int);

	if (data->is_data) {
		memcpy(buf+length, (void*)&(data->data_len), sizeof(unsigned int));
		length += sizeof(unsigned int);
		ret = socket_send(fd, buf, length);

		// send data
		if (ret > 0)
			ret += socket_send_data(fd, data->data, data->data_len);
	}
	else
		ret = socket_send(fd, buf, length);

	return ret;
}

static gboolean __worker_raw_data_pop(pims_ipc_worker_data_s *worker, pims_ipc_raw_data_s **data)
{
	if (!worker)
		return FALSE;

	pthread_mutex_lock(&worker->queue_mutex);
	if (!worker->queue) {
		pthread_mutex_unlock(&worker->queue_mutex);
		*data = NULL;
		return FALSE;
	}

	*data = g_list_first(worker->queue)->data;
	worker->queue = g_list_delete_link(worker->queue, g_list_first(worker->queue));
	pthread_mutex_unlock(&worker->queue_mutex);

	return TRUE;
}

static void* __worker_loop(void *data)
{
	int ret;
	int worker_id;
	int worker_fd;
	pims_ipc_svc_s *ipc_svc = (pims_ipc_svc_s*)data;
	pims_ipc_worker_data_s *worker_data;
	bool disconnected = false;

	worker_fd = eventfd(0, 0);
	if (worker_fd == -1)
		return NULL;
	worker_id = (int)pthread_self();

	worker_data = calloc(1, sizeof(pims_ipc_worker_data_s));
	if (NULL == worker_data) {
		ERROR("calloc() Fail");
		close(worker_fd);
		return NULL;
	}
	worker_data->fd = worker_fd;
	worker_data->worker_id = worker_id;
	worker_data->client_fd = -1;
	worker_data->stop_thread = false;
	pthread_mutex_init(&worker_data->queue_mutex, 0);

	pthread_mutex_lock(&ipc_svc->task_fds_mutex);
	g_hash_table_insert(ipc_svc->task_fds, GINT_TO_POINTER(worker_fd), worker_data);
	pthread_mutex_unlock(&ipc_svc->task_fds_mutex);

	pthread_mutex_lock(&ipc_svc->manager_queue_from_worker_mutex);
	ipc_svc->manager_queue_from_worker = g_list_append(ipc_svc->manager_queue_from_worker, (void*)worker_fd);
	pthread_mutex_unlock(&ipc_svc->manager_queue_from_worker_mutex);

	write_command(ipc_svc->manager, 1);
	DEBUG("worker register to manager : worker_id(%08x00), worker_fd(%d)\n", worker_id, worker_fd);

	struct pollfd *pollfds = (struct pollfd*)calloc(1, sizeof(struct pollfd));
	if (NULL == pollfds) {
		ERROR("calloc() Fail");
		g_hash_table_remove(ipc_svc->task_fds, GINT_TO_POINTER(worker_fd));
		free(worker_data);
		close(worker_fd);
		return NULL;
	}
	pollfds[0].fd = worker_fd;
	pollfds[0].events = POLLIN;

	while (!worker_data->stop_thread) {
		while(1) {
			if (worker_data->stop_thread)
				break;
			ret = poll(pollfds, 1, 3000);	// waiting command from router
			if (ret == -1 && errno == EINTR) {
				continue;
			}
			break;
		}

		if (worker_data->stop_thread)
			break;

		if (ret > 0) {
			pims_ipc_raw_data_s *raw_data = NULL;
			pims_ipc_raw_data_s *result = NULL;

			if (pollfds[0].revents & POLLIN) {
				uint64_t dummy;
				read_command(pollfds[0].fd, &dummy);
				if (__worker_raw_data_pop(worker_data, &raw_data)) {
					pims_ipc_data_h data_in = NULL;
					pims_ipc_data_h data_out = NULL;
					if (strcmp(PIMS_IPC_CALL_ID_CREATE, raw_data->call_id) == 0) {

					}
					else if (strcmp(PIMS_IPC_CALL_ID_DESTROY, raw_data->call_id) == 0) {
						disconnected = true;
					}
					else {
						data_in = pims_ipc_data_steal_unmarshal(raw_data->data, raw_data->data_len);
						raw_data->data = NULL;
						raw_data->data_len = 0;
						raw_data->is_data = false;
						__run_callback(worker_id, raw_data->call_id, data_in, &data_out);
						pims_ipc_data_destroy(data_in);
					}

					if (data_out) {
						__make_raw_data(raw_data->call_id, raw_data->seq_no, data_out, &result);
						pims_ipc_data_destroy(data_out);
					}
					else
						__make_raw_data(raw_data->call_id, raw_data->seq_no, NULL, &result);

					if (worker_data->client_fd != -1)
						__send_raw_data(worker_data->client_fd, raw_data->client_id, result);
					__free_raw_data(raw_data);
					__free_raw_data(result);
				}
			}
		}
	}

	if (!disconnected)
		ERROR("client fd closed, worker_fd : %d", worker_fd);
	INFO("task thread terminated --------------------------- (worker_fd : %d)", worker_fd);

	pthread_mutex_lock(&ipc_svc->client_info_mutex);
	g_hash_table_remove(ipc_svc->worker_client_info_map, GINT_TO_POINTER(worker_id));
	pthread_mutex_unlock(&ipc_svc->client_info_mutex);

	pthread_mutex_lock(&ipc_svc->task_fds_mutex);
	g_hash_table_remove(ipc_svc->task_fds, GINT_TO_POINTER(worker_fd));		// __worker_data_free will be called
	pthread_mutex_unlock(&ipc_svc->task_fds_mutex);

	close(worker_fd);
	free ((void*)pollfds);

	if (_client_disconnected_cb.callback)
		_client_disconnected_cb.callback((pims_ipc_h)worker_id, _client_disconnected_cb.user_data);

	return NULL;
}

static void __launch_thread(void *(*start_routine) (void *), void *data)
{
	pthread_t worker;
	pthread_attr_t attr;

	// set kernel thread
	pthread_attr_init(&attr);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

	pthread_create(&worker, &attr, start_routine, data);
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

static int __get_worker(const char *client_id, int *worker_fd)
{
	ASSERT(client_id);
	ASSERT(worker_fd);

	if (!__is_worker_available()) {
		ERROR("There is no idle worker");
		return -1;
	}
	*worker_fd = (int)(g_list_first(_g_singleton->workers)->data);
	_g_singleton->workers = g_list_delete_link(_g_singleton->workers,
			g_list_first(_g_singleton->workers));

	g_hash_table_insert(_g_singleton->client_worker_map, g_strdup(client_id), GINT_TO_POINTER(*worker_fd));

	return 0;
}

static int __find_worker(const char *client_id, int *worker_fd)
{
	char *orig_pid = NULL;
	int fd;

	ASSERT(client_id);
	ASSERT(worker_fd);

	if (FALSE == g_hash_table_lookup_extended(_g_singleton->client_worker_map, client_id,
				(gpointer*)&orig_pid, (gpointer*)&fd)) {
		VERBOSE("unable to find worker id for %s", client_id);
		return -1;
	}

	*worker_fd = GPOINTER_TO_INT(fd);
	return 0;
}

static bool __request_pop(pims_ipc_request_s *data_queue, pims_ipc_raw_data_s **data)
{
	bool ret = false;
	GList *cursor;

	pthread_mutex_lock(&data_queue->raw_data_mutex);
	cursor = g_list_first(data_queue->raw_data);
	if (cursor) {
		*data = cursor->data;
		data_queue->raw_data = g_list_delete_link(data_queue->raw_data, cursor);
		(data_queue->request_count)--;

		ret = true;
	}
	else
		*data = NULL;

	pthread_mutex_unlock(&data_queue->raw_data_mutex);
	return ret;
}

static bool __worker_raw_data_push(pims_ipc_worker_data_s *worker_data, int client_fd, pims_ipc_raw_data_s *data)
{
	pthread_mutex_lock(&worker_data->queue_mutex);
	worker_data->queue = g_list_append(worker_data->queue, data);
	worker_data->client_fd = client_fd;
	pthread_mutex_unlock(&worker_data->queue_mutex);

	return true;
}

static int _find_worker_id(pims_ipc_svc_s *ipc_svc, int worker_fd)
{
	int worker_id = 0;
	pims_ipc_worker_data_s *worker_data = NULL;
	pthread_mutex_lock(&ipc_svc->task_fds_mutex);
	worker_data = g_hash_table_lookup(ipc_svc->task_fds, GINT_TO_POINTER(worker_fd));
	if (NULL == worker_data) {
		ERROR("g_hash_table_lookup(%d) return NULL", worker_fd);
		pthread_mutex_unlock(&ipc_svc->task_fds_mutex);
		return -1;
	}
	worker_id = worker_data->worker_id;
	pthread_mutex_unlock(&ipc_svc->task_fds_mutex);
	return worker_id;
}

static int _create_client_info(int fd, pims_ipc_client_info_s **p_client_info)
{
	int ret;
	pid_t pid;
	char errmsg[1024] = {0};

	pims_ipc_client_info_s *client_info = calloc(1, sizeof(pims_ipc_client_info_s));
	if (NULL == client_info) {
		ERROR("calloc() return NULL");
		return -1;
	}

	ret = cynara_creds_socket_get_client(fd, CLIENT_METHOD_SMACK, &(client_info->smack));
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERROR("cynara_creds_socket_get_client() Fail(%d,%s)", ret, errmsg);
		_destroy_client_info(client_info);
		return -1;
	}

	ret = cynara_creds_socket_get_user(fd, USER_METHOD_UID, &(client_info->uid));
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERROR("cynara_creds_socket_get_user() Fail(%d,%s)", ret, errmsg);
		_destroy_client_info(client_info);
		return -1;
	}

	ret = cynara_creds_socket_get_pid(fd, &pid);
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERROR("cynara_creds_socket_get_pid() Fail(%d,%s)", ret, errmsg);
		_destroy_client_info(client_info);
		return -1;
	}

	client_info->client_session = cynara_session_from_pid(pid);
	if (NULL == client_info->client_session) {
		ERROR("cynara_session_from_pid() return NULL");
		_destroy_client_info(client_info);
		return -1;
	}
	*p_client_info = client_info;

	return 0;
}

static pims_ipc_client_info_s* _clone_client_info(pims_ipc_client_info_s *client_info)
{
	if (NULL == client_info) {
		ERROR("client_info is NULL");
		return NULL;
	}

	pims_ipc_client_info_s *clone = calloc(1, sizeof(pims_ipc_client_info_s));
	if (NULL == clone) {
		ERROR("calloc() Fail");
		return NULL;
	}

	if (client_info->smack) {
		clone->smack = strdup(client_info->smack);
		if (NULL == clone->smack) {
			ERROR("strdup() Fail");
			_destroy_client_info(clone);
			return NULL;
		}
	}

	if (client_info->uid) {
		clone->uid = strdup(client_info->uid);
		if (NULL == clone->uid) {
			ERROR("strdup() Fail");
			_destroy_client_info(clone);
			return NULL;
		}
	}

	if (client_info->client_session) {
		clone->client_session = strdup(client_info->client_session);
		if (NULL == clone->client_session) {
			ERROR("strdup() Fail");
			_destroy_client_info(clone);
			return NULL;
		}
	}

	return clone;
}


static int __process_router_event(pims_ipc_svc_s *ipc_svc, gboolean for_queue)
{
	gboolean is_valid = FALSE;
	pims_ipc_request_s *data_queue = NULL;
	GList *queue_cursor = NULL;
	int worker_fd = 0;
	char *client_id = NULL;
	int *org_fd;
	int ret;

	do {
		pthread_mutex_lock(&ipc_svc->request_data_queue_mutex);
		queue_cursor = g_list_first(ipc_svc->request_queue);
		if (NULL == queue_cursor) {
			pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);
			return 0;
		}
		client_id = (char *)(queue_cursor->data);
		ASSERT(client_id != NULL);
		pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);

		ret = g_hash_table_lookup_extended(ipc_svc->request_data_queue, (void*)client_id, (gpointer*)&org_fd, (gpointer*)&data_queue);

		if (for_queue)
			ipc_svc->delay_count--;

		if (ret == TRUE && data_queue) {
			int *org_fd;
			pims_ipc_worker_data_s *worker_data = NULL;

			pthread_mutex_lock(&data_queue->raw_data_mutex);
			GList *cursor = g_list_first(data_queue->raw_data);
			if (!cursor) {
				pthread_mutex_unlock(&data_queue->raw_data_mutex);
				break;
			}

			pims_ipc_raw_data_s *data = (pims_ipc_raw_data_s*)(cursor->data);
			if (NULL == data) {
				ERROR("data is NULL");
				pthread_mutex_unlock(&data_queue->raw_data_mutex);
				break;
			}
			char *call_id = data->call_id;
			int client_fd = data_queue->client_fd;

			ASSERT(call_id != NULL);

			VERBOSE("call_id = [%s]", call_id);
			if (strcmp(PIMS_IPC_CALL_ID_CREATE, call_id) == 0) {
				// Get a worker. If cannot get a worker, create a worker and enqueue a current request
				__launch_thread(__worker_loop, ipc_svc);
				if (__get_worker((const char*)client_id, &worker_fd) != 0) {
					ipc_svc->delay_count++;
					pthread_mutex_unlock(&data_queue->raw_data_mutex);
					is_valid = TRUE;
					break;
				}
				int worker_id = _find_worker_id(ipc_svc, worker_fd);
				pthread_mutex_lock(&ipc_svc->client_info_mutex);
				pims_ipc_client_info_s *client_info = g_hash_table_lookup(ipc_svc->client_info_map, client_id);
				pims_ipc_client_info_s *client_info_clone = _clone_client_info(client_info);
				g_hash_table_insert(ipc_svc->worker_client_info_map, GINT_TO_POINTER(worker_id), client_info_clone);
				pthread_mutex_unlock(&ipc_svc->client_info_mutex);
			}
			else {
				// Find a worker
				if (__find_worker((const char*)client_id, &worker_fd) != 0) {
					ERROR("unable to find a worker");
					pthread_mutex_unlock(&data_queue->raw_data_mutex);
					break;
				}
			}
			pthread_mutex_unlock(&data_queue->raw_data_mutex);

			VERBOSE("routing client_id : %s, seq_no: %d, client_fd = %d, worker fd = %d", client_id, data->seq_no, client_fd, worker_fd);

			if (worker_fd <= 0)
				break;

			pthread_mutex_lock(&ipc_svc->task_fds_mutex);
			if (FALSE == g_hash_table_lookup_extended(ipc_svc->task_fds,
						GINT_TO_POINTER(worker_fd), (gpointer*)&org_fd, (gpointer*)&worker_data)) {
				ERROR("hash lookup fail : worker_fd (%d)", worker_fd);
				pthread_mutex_unlock(&ipc_svc->task_fds_mutex);
				break;
			}

			if (__request_pop(data_queue, &data)) {
				__worker_raw_data_push(worker_data, client_fd, data);
				write_command(worker_fd, 1);
			}

			pthread_mutex_unlock(&ipc_svc->task_fds_mutex);
		}

		pthread_mutex_lock(&ipc_svc->request_data_queue_mutex);
		free(client_id);
		ipc_svc->request_queue = g_list_delete_link(ipc_svc->request_queue, queue_cursor);
		pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);

		is_valid = TRUE;
	} while (0);

	if (is_valid == FALSE)
		return -1;

	return 1;
}

static int __process_manager_event(pims_ipc_svc_s *ipc_svc)
{
	GList *cursor = NULL;
	int worker_fd;

	// client socket terminated without disconnect request
	pthread_mutex_lock(&ipc_svc->manager_queue_from_epoll_mutex);
	if (ipc_svc->manager_queue_from_epoll) {
		cursor = g_list_first(ipc_svc->manager_queue_from_epoll);
		char *client_id = (char*)cursor->data;
		__find_worker(client_id, &worker_fd);

		ipc_svc->manager_queue_from_epoll = g_list_delete_link(ipc_svc->manager_queue_from_epoll, cursor);
		pthread_mutex_unlock(&ipc_svc->manager_queue_from_epoll_mutex);

		// remove client_fd
		g_hash_table_remove(ipc_svc->client_worker_map, client_id);
		free(client_id);

		// stop worker thread
		if (worker_fd) {
			int *org_fd;
			pims_ipc_worker_data_s *worker_data;

			pthread_mutex_lock(&ipc_svc->task_fds_mutex);
			if (FALSE == g_hash_table_lookup_extended(ipc_svc->task_fds,
						GINT_TO_POINTER(worker_fd), (gpointer*)&org_fd, (gpointer*)&worker_data)) {
				ERROR("g_hash_table_lookup_extended fail : worker_fd (%d)", worker_fd);
				pthread_mutex_unlock(&ipc_svc->task_fds_mutex);
				return -1;
			}
			worker_data->stop_thread = true;
			worker_data->client_fd = -1;
			pthread_mutex_unlock(&ipc_svc->task_fds_mutex);

			write_command(worker_fd, 1);
			VERBOSE("write command to worker terminate (worker_fd : %d)", worker_fd);
		}
		return 0;
	}
	pthread_mutex_unlock(&ipc_svc->manager_queue_from_epoll_mutex);

	// create new worker
	pthread_mutex_lock(&ipc_svc->manager_queue_from_worker_mutex);
	if (ipc_svc->manager_queue_from_worker) {

		cursor = g_list_first(ipc_svc->manager_queue_from_worker);
		while (cursor) {
			worker_fd = (int)cursor->data;
			ipc_svc->manager_queue_from_worker = g_list_delete_link(ipc_svc->manager_queue_from_worker, cursor);

			if (worker_fd) {
				DEBUG("add idle worker_fd : %d", worker_fd);
				ipc_svc->workers = g_list_append(ipc_svc->workers, (void*)worker_fd);
			}
			cursor = g_list_first(ipc_svc->manager_queue_from_worker);
		}
		pthread_mutex_unlock(&ipc_svc->manager_queue_from_worker_mutex);
		return 0;
	}
	pthread_mutex_unlock(&ipc_svc->manager_queue_from_worker_mutex);

	return 0;
}

// if delete = true, steal client_id, then free(client_id)
// if delete = false, return client_id pointer, then do no call free(client_id
static int __find_client_id(pims_ipc_svc_s *ipc_svc, int client_fd, bool delete, char **client_id)
{
	pims_ipc_client_map_s *client;
	GList *cursor = NULL;
	cursor = g_list_first(ipc_svc->client_id_fd_map);
	while(cursor) {
		client = cursor->data;
		if (client->fd == client_fd) {
			*client_id = client->id;
			if (delete) {
				client->id = NULL;
				ipc_svc->client_id_fd_map = g_list_delete_link(ipc_svc->client_id_fd_map, cursor);			//free(client);
				free(client);
			}
			return 0;
		}
		cursor = g_list_next(cursor);
	}
	return -1;
}

static void __request_push(pims_ipc_svc_s *ipc_svc, char *client_id, int client_fd, pims_ipc_raw_data_s *data)
{
	int ret;
	int *org_fd;
	pims_ipc_request_s *data_queue = NULL;
	if (NULL == data) {
		ERROR("data is NULL");
		return;
	}

	pthread_mutex_lock(&ipc_svc->request_data_queue_mutex);
	ret = g_hash_table_lookup_extended(ipc_svc->request_data_queue, (void*)client_id, (gpointer*)&org_fd,(gpointer*)&data_queue);
	if (ret == TRUE && data_queue) {
	}
	else {
		data_queue = calloc(1, sizeof(pims_ipc_request_s));
		if (NULL == data_queue) {
			ERROR("calloc() Fail");
			pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);
			return;
		}
		data_queue->request_count = 0;
		pthread_mutex_init(&data_queue->raw_data_mutex, 0);

		g_hash_table_insert(ipc_svc->request_data_queue, g_strdup(client_id), data_queue);
	}
	ipc_svc->request_queue = g_list_append(ipc_svc->request_queue, g_strdup(client_id));
	pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);

	pthread_mutex_lock(&data_queue->raw_data_mutex);
	data_queue->raw_data = g_list_append(data_queue->raw_data, data);
	data_queue->client_fd = client_fd;
	data_queue->request_count++;
	pthread_mutex_unlock(&data_queue->raw_data_mutex);
}

static void __delete_request_queue(pims_ipc_svc_s *ipc_svc, char *client_id)
{
	pims_ipc_request_s *data_queue = NULL;
	int ret;
	int *org_fd;
	GList *l;
	GList *cursor;

	pthread_mutex_lock(&ipc_svc->request_data_queue_mutex);
	ret = g_hash_table_lookup_extended(ipc_svc->request_data_queue, (void*)client_id, (gpointer*)&org_fd,(gpointer*)&data_queue);
	if (ret == TRUE)
		g_hash_table_remove(ipc_svc->request_data_queue, (void*)client_id);

	cursor = g_list_first(ipc_svc->request_queue);
	while (cursor) {
		l = cursor;
		char *id = l->data;
		cursor = g_list_next(cursor);
		if (id && strcmp(id, client_id) == 0) {
			free(id);
			ipc_svc->request_queue = g_list_delete_link(ipc_svc->request_queue, l);
		}
	}
	pthread_mutex_unlock(&ipc_svc->request_data_queue_mutex);

	if (data_queue) {
		pthread_mutex_lock(&data_queue->raw_data_mutex);
		cursor = g_list_first(data_queue->raw_data);
		pims_ipc_raw_data_s *data;
		while(cursor) {
			l = cursor;
			data = (pims_ipc_raw_data_s *)cursor->data;
			cursor = g_list_next(cursor);
			data_queue->raw_data = g_list_delete_link(data_queue->raw_data, l);
			__free_raw_data(data);
		}
		g_list_free(data_queue->raw_data);
		pthread_mutex_unlock(&data_queue->raw_data_mutex);
		pthread_mutex_destroy(&data_queue->raw_data_mutex);
		free(data_queue);
	}
}

static int __send_identify(int fd, unsigned int seq_no, char *id, int id_len)
{
	int len = sizeof(unsigned int)					// total size
		+ id_len + sizeof(unsigned int)		// id
		+ sizeof(unsigned int);				// seq_no

	char buf[len+1];

	int length = 0;
	memset(buf, 0x0, len+1);

	// total len
	memcpy(buf, (void*)&len, sizeof(unsigned int));
	length += sizeof(unsigned int);

	// id
	memcpy(buf+length, (void*)&(id_len), sizeof(unsigned int));
	length += sizeof(unsigned int);
	memcpy(buf+length, (void*)(id), id_len);
	length += id_len;

	// seq_no
	memcpy(buf+length, (void*)&(seq_no), sizeof(unsigned int));
	length += sizeof(unsigned int);

	return socket_send(fd, buf, length);
}

static int __recv_raw_data(int fd, pims_ipc_raw_data_s **data, bool *identity)
{
	int len = 0;
	pims_ipc_raw_data_s *temp;

	/* read the size of message. note that ioctl is non-blocking */
	if (ioctl(fd, FIONREAD, &len)) {
		ERROR("ioctl failed: %d", errno);
		return -1;
	}

	/* when server or client closed socket */
	if (len == 0) {
		INFO("[IPC Socket] connection is closed");
		return 0;
	}

	temp = (pims_ipc_raw_data_s*)calloc(1, sizeof(pims_ipc_raw_data_s));
	if (NULL == temp) {
		ERROR("calloc() Fail");
		return -1;
	}
	temp->client_id = NULL;
	temp->client_id_len = 0;
	temp->call_id = NULL;
	temp->call_id_len = 0;
	temp->seq_no = 0;
	temp->is_data = FALSE;
	temp->data = NULL;
	temp->data_len = 0;

	int ret = 0;
	int read_len = 0;
	unsigned int total_len = 0;
	unsigned int is_data = FALSE;

	do {
		// total length
		ret = TEMP_FAILURE_RETRY(read(fd, (void *)&total_len, sizeof(unsigned int)));
		if (ret < 0) {	 ERROR("read error"); break;		}
		read_len += ret;

		// client_id
		ret  = TEMP_FAILURE_RETRY(read(fd, (void *)&(temp->client_id_len), sizeof(unsigned int)));
		if (ret < 0) {	 ERROR("read error"); break;		}
		read_len += ret;

		temp->client_id = calloc(1, temp->client_id_len+1);
		if (NULL == temp->client_id) {
			ERROR("calloc() Fail");
			ret = -1;
			break;
		}
		ret = socket_recv(fd, (void *)&(temp->client_id), temp->client_id_len);
		if (ret < 0) {
			ERROR("socket_recv error(%d)", ret);
			break;
		}
		read_len += ret;

		// sequnce no
		ret = TEMP_FAILURE_RETRY(read(fd, (void *)&(temp->seq_no), sizeof(unsigned int)));
		if (ret < 0) {	 ERROR("read error"); break;		}
		read_len += ret;

		if (total_len == read_len) {
			*data = temp;
			*identity = true;
			return read_len;
		}

		// call_id
		ret  = TEMP_FAILURE_RETRY(read(fd, (void *)&(temp->call_id_len), sizeof(unsigned int)));
		if (ret < 0)	{ ERROR("read error"); break;		}
		read_len += ret;

		temp->call_id = calloc(1, temp->call_id_len+1);
		if (NULL == temp->call_id) {
			ERROR("calloc() Fail");
			ret = -1;
			break;
		}
		ret = socket_recv(fd, (void *)&(temp->call_id), temp->call_id_len);
		if (ret < 0) {
			ERROR("socket_recv error(%d)", ret);
			break;
		}
		read_len += ret;

		// is_data
		ret = TEMP_FAILURE_RETRY(read(fd, (void *)&(is_data), sizeof(unsigned int)));
		if (ret < 0) {	 ERROR("read error"); break;		}
		read_len += ret;

		// data
		if (is_data) {
			temp->is_data = TRUE;
			ret = TEMP_FAILURE_RETRY(read(fd, (void *)&(temp->data_len), sizeof(unsigned int)));
			if (ret < 0) {	ERROR("read error"); break;		}
			read_len += ret;

			temp->data = calloc(1, temp->data_len+1);
			if (NULL == temp->data) {
				ERROR("calloc() Fail");
				ret = -1;
				break;
			}
			ret = socket_recv(fd, (void *)&(temp->data), temp->data_len);
			if (ret < 0) {
				ERROR("socket_recv error(%d)", ret);
				break;
			}
			read_len += ret;
		}

		INFO("client_id : %s, call_id : %s, seq_no : %d", temp->client_id, temp->call_id, temp->seq_no);

		*data = temp;
		*identity = false;
	} while(0);

	if (ret < 0) {
		ERROR("total_len(%d) client_id_len(%d)", total_len, temp->client_id_len);
		__free_raw_data(temp);
		*data = NULL;
		*identity = false;
		return -1;
	}

	return read_len;
}

static gboolean __request_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	int ret;
	int event_fd = g_io_channel_unix_get_fd(src);
	char *client_id = NULL;
	pims_ipc_svc_s *ipc_svc = (pims_ipc_svc_s*)data;
	if (NULL == ipc_svc) {
		ERROR("ipc_svc is NULL");
		return FALSE;
	}

	if (G_IO_HUP & condition) {
		INFO("client closed ------------------------client_fd : %d", event_fd);

		close(event_fd);

		// Find client_id
		__find_client_id(ipc_svc, event_fd, true, &client_id);

		// Send client_id to manager to terminate worker thread
		if (client_id) {
			pthread_mutex_lock(&ipc_svc->client_info_mutex);
			g_hash_table_remove(ipc_svc->client_info_map, client_id);
			pthread_mutex_unlock(&ipc_svc->client_info_mutex);

			pthread_mutex_lock(&ipc_svc->manager_queue_from_epoll_mutex);
			ipc_svc->manager_queue_from_epoll = g_list_append(ipc_svc->manager_queue_from_epoll, (void*)g_strdup(client_id));
			pthread_mutex_unlock(&ipc_svc->manager_queue_from_epoll_mutex);
			write_command(ipc_svc->manager, 1);
			__delete_request_queue(ipc_svc, client_id);
			free(client_id);
		}

		return FALSE;
	}

	// receive data from client
	int recv_len;
	bool identity = false;
	pims_ipc_raw_data_s *req = NULL;

	recv_len = __recv_raw_data(event_fd, &req, &identity);
	if (recv_len > 0) {
		// send command to router
		if (identity) {
			pims_ipc_client_map_s *client = (pims_ipc_client_map_s*)calloc(1, sizeof(pims_ipc_client_map_s));
			if (NULL == client) {
				ERROR("calloc() Fail");
				close(event_fd);
				return FALSE;
			}

			client->fd = event_fd;

			char temp[100];
			snprintf(temp, sizeof(temp), "%d_%s", ipc_svc->unique_sequence_number++, req->client_id);
			client->id = strdup(temp);
			free(req->client_id);
			req->client_id = NULL;
			ipc_svc->client_id_fd_map = g_list_append(ipc_svc->client_id_fd_map, client);

			// send server pid to client
			snprintf(temp, sizeof(temp), "%x", getpid());
			ret = __send_identify(event_fd, req->seq_no, temp, strlen(temp));

			__free_raw_data(req);
			if (ret < 0) {
				ERROR("__send_identify() Fail(%d)", ret);
				close(event_fd);
				return FALSE;
			}

			pims_ipc_client_info_s *client_info = NULL;
			if (0 != _create_client_info(event_fd, &client_info))
				ERROR("_create_client_info() Fail");
			pthread_mutex_lock(&ipc_svc->client_info_mutex);
			g_hash_table_insert(ipc_svc->client_info_map, g_strdup(client->id), client_info);
			pthread_mutex_unlock(&ipc_svc->client_info_mutex);

			return TRUE;
		}

		__find_client_id(ipc_svc, event_fd, false, &client_id);

		if (client_id) {
			__request_push(ipc_svc, client_id, event_fd, req);
			write_command(ipc_svc->router, 1);
		}
		else
			ERROR("__find_client_id fail : event_fd (%d)", event_fd);
	}
	else {
		ERROR("receive invalid : %d", event_fd);
		close(event_fd);
		return FALSE;
	}

	return TRUE;
}

static gboolean __socket_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	GIOChannel *channel;
	pims_ipc_svc_s *ipc_svc = (pims_ipc_svc_s*)data;
	int client_sockfd = -1;
	int sockfd = ipc_svc->sockfd;
	struct sockaddr_un clientaddr;
	socklen_t client_len = sizeof(clientaddr);

	client_sockfd = accept(sockfd, (struct sockaddr *)&clientaddr, &client_len);
	if (-1 == client_sockfd) {
		ERROR("accept error : %s", strerror(errno));
		return TRUE;
	}

	channel = g_io_channel_unix_new(client_sockfd);
	g_io_add_watch(channel, G_IO_IN|G_IO_HUP, __request_handler, data);
	g_io_channel_unref(channel);

	return TRUE;
}

static void* __main_loop(void *user_data)
{
	int ret;
	struct sockaddr_un addr;
	GIOChannel *gio = NULL;
	pims_ipc_svc_s *ipc_svc = (pims_ipc_svc_s*)user_data;

	if (sd_listen_fds(1) == 1 && sd_is_socket_unix(SD_LISTEN_FDS_START, SOCK_STREAM, -1, ipc_svc->service, 0) > 0) {
		 ipc_svc->sockfd = SD_LISTEN_FDS_START;
	}
	else {
		unlink(ipc_svc->service);
		ipc_svc->sockfd = socket(PF_UNIX, SOCK_STREAM, 0);

		bzero(&addr, sizeof(addr));
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_svc->service);

		ret = bind(ipc_svc->sockfd, (struct sockaddr *)&addr, sizeof(addr));
		if (ret != 0)
			ERROR("bind error :%d", ret);
		ret = listen(ipc_svc->sockfd, 30);

		ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
		ret = chmod(ipc_svc->service, ipc_svc->mode);
	}

	gio = g_io_channel_unix_new(ipc_svc->sockfd);

	g_io_add_watch(gio, G_IO_IN, __socket_handler, (gpointer)ipc_svc);

	return NULL;
}

static int __open_router_fd(pims_ipc_svc_s *ipc_svc)
{
	int ret = -1;
	int flags;
	int router;
	int manager;

	// router inproc eventfd
	router = eventfd(0,0);
	if (-1 == router) {
		ERROR("eventfd error : %d", errno);
		return -1;
	}
	VERBOSE("router :%d\n", router);

	flags = fcntl(router, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	ret = fcntl (router, F_SETFL, flags | O_NONBLOCK);
	VERBOSE("rounter fcntl : %d\n", ret);

	// manager inproc eventfd
	manager = eventfd(0,0);
	if (-1 == manager) {
		ERROR("eventfd error : %d", errno);
		close(router);
		return -1;
	}
	VERBOSE("manager :%d\n", manager);

	flags = fcntl(manager, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	ret = fcntl (manager, F_SETFL, flags | O_NONBLOCK);
	VERBOSE("manager fcntl : %d\n", ret);

	ipc_svc->router = router;
	ipc_svc->manager = manager;

	return 0;
}

static void __close_router_fd(pims_ipc_svc_s *ipc_svc)
{
	close(ipc_svc->router);
	close(ipc_svc->manager);
}

static void* __publish_loop(void *user_data)
{
	int ret;
	int epfd;

	struct sockaddr_un addr;
	struct epoll_event ev = {0};
	pims_ipc_svc_for_publish_s *ipc_svc = (pims_ipc_svc_for_publish_s*)user_data;

	unlink(ipc_svc->service);
	ipc_svc->publish_sockfd = socket(PF_UNIX, SOCK_STREAM, 0);

	bzero(&addr, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_svc->service);

	int flags = fcntl (ipc_svc->publish_sockfd, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	ret = fcntl (ipc_svc->publish_sockfd, F_SETFL, flags | O_NONBLOCK);
	VERBOSE("publish socketfd fcntl : %d\n", ret);

	ret = bind(ipc_svc->publish_sockfd, (struct sockaddr *)&(addr), sizeof(struct sockaddr_un));
	if (ret != 0)
		ERROR("bind error :%d", ret);
	ret = listen(ipc_svc->publish_sockfd, 30);
	WARN_IF(ret != 0, "listen error :%d", ret);

	ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
	WARN_IF(ret != 0, "chown error :%d", ret);
	ret = chmod(ipc_svc->service, ipc_svc->mode);
	WARN_IF(ret != 0, "chmod error :%d", ret);

	epfd = epoll_create(MAX_EPOLL_EVENT);

	ev.events = EPOLLIN | EPOLLHUP;
	ev.data.fd = ipc_svc->publish_sockfd;

	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, ipc_svc->publish_sockfd, &ev);
	WARN_IF(ret != 0, "listen error :%d", ret);

	while (!ipc_svc->epoll_stop_thread) {
		int i = 0;
		struct epoll_event events[MAX_EPOLL_EVENT] = {{0}, };
		int event_num = epoll_wait(epfd, events, MAX_EPOLL_EVENT, -1);

		if (ipc_svc->epoll_stop_thread)
			break;

		if (event_num == -1) {
			if (errno != EINTR) {
				ERROR("errno:%d\n", errno);
				break;
			}
		}

		for (i = 0; i < event_num; i++) {
			int event_fd = events[i].data.fd;

			if (events[i].events & EPOLLHUP) {
				VERBOSE("client closed -----------------------------------------:%d", event_fd);
				if (epoll_ctl(epfd, EPOLL_CTL_DEL, event_fd, events) == -1) {
					ERROR("epoll_ctl (EPOLL_CTL_DEL) fail : errno(%d)", errno);
				}
				close(event_fd);

				// Find client_id and delete
				GList *cursor = NULL;

				pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
				cursor = g_list_first(ipc_svc->subscribe_fds);
				while (cursor) {
					if (event_fd == (int)cursor->data) {
						ipc_svc->subscribe_fds = g_list_delete_link(ipc_svc->subscribe_fds, cursor);
						break;
					}
					cursor = g_list_next(cursor);
				}
				pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);
				continue;
			}
			else if (event_fd == ipc_svc->publish_sockfd) {		// connect client
				struct sockaddr_un remote;
				int remote_len = sizeof(remote);
				int client_fd = accept(ipc_svc->publish_sockfd, (struct sockaddr *)&remote, (socklen_t*) &remote_len);
				if (client_fd == -1) {
					ERROR("accept fail : errno : %d", errno);
					continue;
				}
				VERBOSE("client subscriber connect: %d", client_fd);

				pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
				ipc_svc->subscribe_fds = g_list_append(ipc_svc->subscribe_fds, (void*)client_fd);
				pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);

				ev.events = EPOLLIN;
				ev.data.fd = client_fd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
					ERROR("epoll_ctl (EPOLL_CTL_ADD) fail : error(%d)\n", errno);
					continue;
				}
			}
		}
	}

	close(ipc_svc->publish_sockfd);
	close(epfd);

	return NULL;
}

static void __stop_for_publish(pims_ipc_svc_for_publish_s *ipc_svc)
{
	ipc_svc->epoll_stop_thread = true;
}

static void* __router_loop(void *data)
{
	pims_ipc_svc_s *ipc_svc = (pims_ipc_svc_s*)data;
	int fd_count = 2;
	struct pollfd *pollfds;

	pollfds = (struct pollfd*)calloc(fd_count, sizeof(struct pollfd));
	if (NULL == pollfds) {
		ERROR("calloc() Fail");
		return NULL;
	}
	pollfds[0].fd = ipc_svc->router;
	pollfds[0].events = POLLIN;
	pollfds[1].fd = ipc_svc->manager;
	pollfds[1].events = POLLIN;

	while (1) {
		int ret = -1;
		uint64_t dummy;
		int check_router_queue = -1;
		int check_manager_queue = -1;

		while (1) {
			ret = poll(pollfds, fd_count, 1000);
			if (ret == -1 && errno == EINTR) {
				//free (pollfds);
				continue;		//return NULL;
			}
			break;
		}

		if (ret > 0) {
			if (pollfds[0].revents & POLLIN) {
				// request router: send request to worker
				if (sizeof (dummy) == read_command(pollfds[0].fd, &dummy)) {
					check_router_queue = __process_router_event(ipc_svc, false);
				}
			}

			if (pollfds[1].revents & POLLIN) {
				// worker manager
				if (sizeof (dummy) == read_command(pollfds[1].fd, &dummy)) {
					check_manager_queue = __process_manager_event(ipc_svc);
					if (ipc_svc->delay_count > 0)
						check_router_queue = __process_router_event(ipc_svc, true);
				}
			}
		}

		// check queue
		while(check_router_queue > 0 || check_manager_queue > 0) {
			read_command(pollfds[0].fd, &dummy);
			check_router_queue = __process_router_event(ipc_svc, false);

			read_command(pollfds[1].fd, &dummy);
			check_manager_queue = __process_manager_event(ipc_svc);
			if (ipc_svc->delay_count > 0)
				check_router_queue = __process_router_event(ipc_svc, true);
		}
	}

	free(pollfds);

	return NULL;
}

API void pims_ipc_svc_run_main_loop(GMainLoop* loop)
{
	int ret = -1;
	GMainLoop* main_loop = loop;

	if (main_loop == NULL) {
		main_loop = g_main_loop_new(NULL, FALSE);
	}

	if (_g_singleton_for_publish)
		__launch_thread(__publish_loop, _g_singleton_for_publish);

	if (_g_singleton) {
		ret = __open_router_fd(_g_singleton);
		ASSERT(ret == 0);

		int i;
		// launch worker threads in advance
		for (i = 0; i < _g_singleton->workers_max_count; i++)
			__launch_thread(__worker_loop, _g_singleton);

		__launch_thread(__router_loop, _g_singleton);
		__main_loop(_g_singleton);
	}

	g_main_loop_run(main_loop);

	if (_g_singleton)
		__close_router_fd(_g_singleton);

	if (_g_singleton_for_publish)
		__stop_for_publish(_g_singleton_for_publish);

}

API void pims_ipc_svc_set_client_disconnected_cb(pims_ipc_svc_client_disconnected_cb callback, void *user_data)
{
	if (_client_disconnected_cb.callback) {
		ERROR("already registered");
		return;
	}
	_client_disconnected_cb.callback = callback;
	_client_disconnected_cb.user_data = user_data;
}

API bool pims_ipc_svc_check_privilege(pims_ipc_h ipc, char *privilege)
{
	int ret;
	int worker_id = (int)ipc;
	pims_ipc_client_info_s *client_info = NULL;
	pims_ipc_client_info_s *client_info_clone = NULL;

	if (NULL == privilege) {
		ERROR("privilege is NULL");
		return false;
	}

	pthread_mutex_lock(&_g_singleton->client_info_mutex);
	client_info = g_hash_table_lookup(_g_singleton->worker_client_info_map, GINT_TO_POINTER(worker_id));
	if (NULL == client_info) {
		ERROR("client_info is NULL");
		pthread_mutex_unlock(&_g_singleton->client_info_mutex);
		return false;
	}
	client_info_clone = _clone_client_info(client_info);
	pthread_mutex_unlock(&_g_singleton->client_info_mutex);

	if (NULL == client_info_clone) {
		ERROR("client_info_clone is NULL");
		return false;
	}

	pthread_mutex_lock(&_g_singleton->cynara_mutex);
 	ret = cynara_check(_g_singleton->cynara, client_info_clone->smack, client_info_clone->client_session, client_info_clone->uid, privilege);
	pthread_mutex_unlock(&_g_singleton->cynara_mutex);

	_destroy_client_info(client_info_clone);

	if (CYNARA_API_ACCESS_ALLOWED == ret)
		return true;

	return false;
}

API int pims_ipc_svc_get_smack_label(pims_ipc_h ipc, char **p_smack)
{
	pims_ipc_client_info_s *client_info = NULL;
	int worker_id = (int)ipc;

	pthread_mutex_lock(&_g_singleton->client_info_mutex);
	client_info = g_hash_table_lookup(_g_singleton->worker_client_info_map, GINT_TO_POINTER(worker_id));
	if (NULL == client_info) {
		ERROR("g_hash_table_lookup(%d) return NULL", worker_id);
		pthread_mutex_unlock(&_g_singleton->client_info_mutex);
		return -1;
	}

	if (client_info->smack) {
		*p_smack = strdup(client_info->smack);
		if (NULL == *p_smack) {
			ERROR("strdup() return NULL");
			pthread_mutex_unlock(&_g_singleton->client_info_mutex);
			return -1;
		}
	}
	pthread_mutex_unlock(&_g_singleton->client_info_mutex);

	return 0;
}

