/*
 * PIMS IPC
 *
 * Copyright (c) 2012 - 2016 Samsung Electronics Co., Ltd. All rights reserved.
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

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#include <glib.h>

#include "pims-internal.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc-data.h"
#include "pims-socket.h"
#include "pims-ipc-utils.h"
#include "pims-ipc-worker.h"

#define PIMS_IPC_WORKER_THREAD_WAIT_TIME 100 /* milliseconds */

typedef struct {
	pims_ipc_svc_client_disconnected_cb callback;
	void * user_data;
} pims_ipc_svc_client_disconnected_cb_t;

/* idle_worker_pool SHOULD handle on main thread */
static GList *idle_worker_pool;
static GHashTable *worker_cb_table; /* call_id, cb_data */
static __thread pims_ipc_svc_client_disconnected_cb_t _client_disconnected_cb = {NULL, NULL};

static int unique_sequence_number;
static GHashTable *worker_client_info_map; /* key : worker_id, data : pims_ipc_client_info_s* */

int worker_wait_idle_worker_ready(pims_ipc_worker_data_s *worker_data)
{
	struct timespec timeout = {0};

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_nsec += PIMS_IPC_WORKER_THREAD_WAIT_TIME * 1000000;
	timeout.tv_sec += timeout.tv_nsec / 1000000000L;
	timeout.tv_nsec = timeout.tv_nsec % 1000000000L;

	pthread_mutex_lock(&worker_data->ready_mutex);

	if (!worker_data->fd) {
		WARN("worker fd is null, wait until worker thread create done.");
		if (pthread_cond_timedwait(&worker_data->ready, &worker_data->ready_mutex, &timeout)) {
			ERR("Get idle worker timeout Fail!");
			pthread_mutex_unlock(&worker_data->ready_mutex);
			return -1;
		}
	}

	pthread_mutex_unlock(&worker_data->ready_mutex);
	return 0;
}

pims_ipc_worker_data_s* worker_get_idle_worker(pims_ipc_svc_s *ipc_svc,
		const char *client_id)
{
	pims_ipc_worker_data_s *worker_data;

	RETV_IF(NULL == client_id, NULL);
	RETVM_IF(NULL == idle_worker_pool, NULL, "There is no idle worker");

	worker_data = g_hash_table_lookup(ipc_svc->client_worker_map, client_id);
	if (worker_data)
		return worker_data;

	worker_data = idle_worker_pool->data;

	idle_worker_pool = g_list_delete_link(idle_worker_pool, idle_worker_pool);

	if (worker_data)
		g_hash_table_insert(ipc_svc->client_worker_map, g_strdup(client_id), worker_data);

	return worker_data;
}

pims_ipc_worker_data_s* worker_find(pims_ipc_svc_s *ipc_svc, const char *client_id)
{
	pims_ipc_worker_data_s *worker_data;

	RETV_IF(NULL == client_id, NULL);

	if (FALSE == g_hash_table_lookup_extended(ipc_svc->client_worker_map, client_id,
				NULL, (gpointer*)&worker_data)) {
		ERR("g_hash_table_lookup_extended(%s) Fail", client_id);
		return NULL;
	}

	return worker_data;
}

void worker_stop_client_worker(pims_ipc_svc_s *ipc_svc, const char *client_id)
{
	pims_ipc_worker_data_s *worker_data;

	worker_data = worker_find(ipc_svc, client_id);

	/* remove client_fd */
	g_hash_table_remove(ipc_svc->client_worker_map, client_id);

	/* stop worker thread */
	if (worker_data) {
		worker_data->stop_thread = TRUE;
		worker_data->client_fd = -1;
		write_command(worker_data->fd, 1);
		DBG("write command to worker terminate(worker_fd:%d)", worker_data->fd);
	}
}


void worker_free_raw_data(void *data)
{
	pims_ipc_raw_data_s *raw_data = data;

	if (NULL == raw_data)
		return;

	free(raw_data->client_id);
	free(raw_data->call_id);
	free(raw_data->data);
	free(raw_data);
}

void worker_free_data(gpointer data)
{
	pims_ipc_worker_data_s *worker_data = data;

	pthread_mutex_lock(&worker_data->queue_mutex);
	if (worker_data->list)
		g_list_free_full(worker_data->list, worker_free_raw_data);
	pthread_mutex_unlock(&worker_data->queue_mutex);

	pthread_cond_destroy(&worker_data->ready);
	pthread_mutex_destroy(&worker_data->ready_mutex);

	free(worker_data);
}


int worker_push_raw_data(pims_ipc_worker_data_s *worker_data, int client_fd,
		pims_ipc_raw_data_s *data)
{
	pthread_mutex_lock(&worker_data->queue_mutex);
	worker_data->list = g_list_append(worker_data->list, data);
	worker_data->client_fd = client_fd;
	pthread_mutex_unlock(&worker_data->queue_mutex);

	return TRUE;
}

static gboolean worker_pop_raw_data(pims_ipc_worker_data_s *worker,
		pims_ipc_raw_data_s **data)
{
	if (!worker)
		return FALSE;

	pthread_mutex_lock(&worker->queue_mutex);
	if (!worker->list) {
		pthread_mutex_unlock(&worker->queue_mutex);
		*data = NULL;
		return FALSE;
	}

	*data = g_list_first(worker->list)->data;
	worker->list = g_list_delete_link(worker->list, g_list_first(worker->list));
	pthread_mutex_unlock(&worker->queue_mutex);

	return TRUE;
}

int worker_set_callback(char *call_id, pims_ipc_svc_cb_s *cb_data)
{
	return g_hash_table_insert(worker_cb_table, call_id, cb_data);
}


static void __run_callback(int client_pid, char *call_id, pims_ipc_data_h dhandle_in,
		pims_ipc_data_h *dhandle_out)
{
	pims_ipc_svc_cb_s *cb_data = NULL;

	VERBOSE("Call id [%s]", call_id);

	cb_data = g_hash_table_lookup(worker_cb_table, call_id);
	if (cb_data == NULL) {
		VERBOSE("No Data for %s", call_id);
		return;
	}

	/* TODO: client_pid is not valide pims_ipc_h */
	cb_data->callback((pims_ipc_h)client_pid, dhandle_in, dhandle_out, cb_data->user_data);
}

static void __make_raw_data(const char *call_id, int seq_no, pims_ipc_data_h data,
		pims_ipc_raw_data_s **out)
{
	pims_ipc_data_s *data_in = data;
	pims_ipc_raw_data_s *raw_data = NULL;

	RET_IF(NULL == out);
	RET_IF(NULL == call_id);

	raw_data = calloc(1, sizeof(pims_ipc_raw_data_s));
	if (NULL == raw_data) {
		ERR("calloc() Fail(%d)", errno);
		return;
	}

	raw_data->call_id = g_strdup(call_id);
	raw_data->call_id_len = strlen(raw_data->call_id);
	raw_data->seq_no = seq_no;

	if (data_in && 0 < data_in->buf_size) {
		raw_data->has_data = TRUE;
		raw_data->data = calloc(1, data_in->buf_size+1);
		if (NULL == raw_data->data) {
			ERR("calloc() Fail");
			free(raw_data->call_id);
			free(raw_data);
			return;
		}
		memcpy(raw_data->data, data_in->buf, data_in->buf_size);
		raw_data->data_len = data_in->buf_size;
	} else {
		raw_data->has_data = FALSE;
		raw_data->data_len = 0;
		raw_data->data = NULL;
	}
	*out = raw_data;
	return;
}

static int __send_raw_data(int fd, const char *client_id, pims_ipc_raw_data_s *data)
{
	int ret = 0;
	unsigned int len, total_len, client_id_len;

	RETV_IF(NULL == data, -1);
	RETV_IF(NULL == client_id, -1);

	client_id_len = strlen(client_id);

	len = sizeof(total_len) + sizeof(client_id_len) + client_id_len + sizeof(data->seq_no)
		+ data->call_id_len + sizeof(data->call_id) + sizeof(data->has_data);
	total_len = len;

	if (data->has_data) {
		len += sizeof(data->data_len);
		total_len = len + data->data_len;
	}

	INFO("client_id: %s, call_id : %s, seq no :%d, len:%d, total len :%d", client_id,
			data->call_id, data->seq_no, len, total_len);

	int length = 0;
	char buf[len+1];
	memset(buf, 0x0, len+1);

	memcpy(buf, &total_len, sizeof(total_len));
	length += sizeof(total_len);

	memcpy(buf+length, &client_id_len, sizeof(client_id_len));
	length += sizeof(client_id_len);
	memcpy(buf+length, client_id, client_id_len);
	length += client_id_len;

	memcpy(buf+length, &(data->seq_no), sizeof(data->seq_no));
	length += sizeof(data->seq_no);

	memcpy(buf+length, &(data->call_id_len), sizeof(data->call_id_len));
	length += sizeof(data->call_id_len);
	memcpy(buf+length, data->call_id, data->call_id_len);
	length += data->call_id_len;

	memcpy(buf+length, &(data->has_data), sizeof(data->has_data));
	length += sizeof(data->has_data);

	if (data->has_data) {
		memcpy(buf+length, &(data->data_len), sizeof(data->data_len));
		length += sizeof(data->data_len);
		ret = socket_send(fd, buf, length);

		/* send data */
		if (ret > 0)
			ret += socket_send_data(fd, data->data, data->data_len);
	} else {
		ret = socket_send(fd, buf, length);
	}

	return ret;
}

static int _get_pid_from_fd(int fd, int *pid)
{
	struct ucred uc;
	socklen_t uc_len = sizeof(uc);
	if (NULL == pid) {
		ERR("Invalid parameter: pid is NULL");
		return -1;
	}

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &uc_len) < 0) {
		ERR("getsockopt() Failed(%d)", errno);
		return -1;
	}

	DBG("Client PID(%d)", uc.pid);
	*pid = uc.pid;

	return 0;
}

static int __worker_loop_handle_raw_data(pims_ipc_worker_data_s *worker_data)
{
	int disconnected = FALSE;
	pims_ipc_data_h data_in = NULL;
	pims_ipc_data_h data_out = NULL;
	pims_ipc_raw_data_s *result = NULL;
	pims_ipc_raw_data_s *raw_data = NULL;

	if (FALSE == worker_pop_raw_data(worker_data, &raw_data))
		return disconnected;

	int ret = 0;
	int client_pid = 0;
	ret = _get_pid_from_fd(worker_data->client_fd, &client_pid);

	if (UTILS_STR_EQUAL == strcmp(PIMS_IPC_CALL_ID_CREATE, raw_data->call_id)) {
		client_register_info(worker_data->client_fd, client_pid);

	} else if (UTILS_STR_EQUAL == strcmp(PIMS_IPC_CALL_ID_DESTROY, raw_data->call_id)) {
		disconnected = TRUE;
	} else {
		data_in = pims_ipc_data_steal_unmarshal(raw_data->data, raw_data->data_len);

		__run_callback(client_pid, raw_data->call_id, data_in, &data_out);
		pims_ipc_data_destroy(data_in);
	}

	if (data_out) {
		__make_raw_data(raw_data->call_id, raw_data->seq_no, data_out, &result);
		pims_ipc_data_destroy(data_out);
	} else
		__make_raw_data(raw_data->call_id, raw_data->seq_no, NULL, &result);

	if (worker_data->client_fd != -1)
		__send_raw_data(worker_data->client_fd, raw_data->client_id, result);
	worker_free_raw_data(raw_data);
	worker_free_raw_data(result);

	return disconnected;
}

static void* __worker_loop(void *data)
{
	int ret;
	pthread_t pid;
	int worker_fd;
	int disconnected = FALSE;
	pims_ipc_worker_data_s *worker_data = data;

	RETV_IF(NULL == data, NULL);

	worker_fd = eventfd(0, 0);
	if (worker_fd == -1)
		return NULL;

	INFO("worker Created ********** worker_fd = %d ***********", worker_fd);

	pid = pthread_self();
	worker_data->client_fd = -1;
	worker_data->stop_thread = FALSE;
	pthread_mutex_lock(&worker_data->ready_mutex);
	worker_data->fd = worker_fd;
	pthread_cond_signal(&worker_data->ready);
	pthread_mutex_unlock(&worker_data->ready_mutex);

	struct pollfd pollfds[1];
	pollfds[0].fd = worker_fd;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;

	while (!worker_data->stop_thread) {
		ret = poll(pollfds, 1, 3000); /* waiting command from router */
		if (-1 == ret) {
			if (errno != EINTR)
				ERR("poll() Fail(%d)", errno);
			continue;
		}
		if (worker_data->stop_thread)
			break;

		if (0 == ret)
			continue;

		if (pollfds[0].revents & POLLIN) {
			uint64_t dummy;
			read_command(pollfds[0].fd, &dummy);

			disconnected = __worker_loop_handle_raw_data(worker_data);
		}
	}

	if (!disconnected)
		ERR("client fd closed, worker_fd : %d", worker_fd);
	INFO("task thread terminated --------------------------- (worker_fd : %d)", worker_fd);

	int client_pid = 0;
	ret = _get_pid_from_fd(worker_data->client_fd, &client_pid);
	if (0 == ret) {
		/*	pthread_mutex_lock(&worker_data->client_mutex); */
		g_hash_table_remove(worker_client_info_map, GINT_TO_POINTER(client_pid));
		DBG("----------------removed(%d)", client_pid);
		/*	pthread_mutex_unlock(&worker_data->client_mutex); */
	}

	worker_free_data(worker_data);
	close(worker_fd);

	if (_client_disconnected_cb.callback)
		_client_disconnected_cb.callback((pims_ipc_h)pid, _client_disconnected_cb.user_data);

	return NULL;
}

void worker_start_idle_worker(pims_ipc_svc_s *ipc_data)
{
	int i;
	pims_ipc_worker_data_s *worker_data;

	for (i = g_list_length(idle_worker_pool); i < ipc_data->workers_max_count; i++) {
		worker_data = calloc(1, sizeof(pims_ipc_worker_data_s));
		if (NULL == worker_data) {
			ERR("calloc() Fail(%d)", errno);
			continue;
		}
		pthread_mutex_init(&worker_data->queue_mutex, 0);
		pthread_mutex_init(&worker_data->ready_mutex, NULL);
		pthread_cond_init(&worker_data->ready, NULL);
		/* pthread_mutex_init(&worker_data->client_mutex, 0); */

		utils_launch_thread(__worker_loop, worker_data);
		idle_worker_pool = g_list_append(idle_worker_pool, worker_data);
	}
}

void worker_stop_idle_worker()
{
	GList *cursor;
	pims_ipc_worker_data_s *worker_data;

	cursor = idle_worker_pool;
	while (cursor) {
		worker_data = cursor->data;
		worker_data->stop_thread = TRUE;
		write_command(worker_data->fd, 1);
		cursor = cursor->next;
	}
}

void worker_init()
{
	worker_cb_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	WARN_IF(NULL == worker_cb_table, "worker cb table is NULL");
}

void worker_deinit()
{
	g_list_free(idle_worker_pool);
	idle_worker_pool = NULL;

	g_hash_table_destroy(worker_cb_table);
	worker_cb_table = NULL;
}

API void pims_ipc_svc_set_client_disconnected_cb(
		pims_ipc_svc_client_disconnected_cb callback, void *user_data)
{
	if (_client_disconnected_cb.callback) {
		ERR("already registered");
		return;
	}
	_client_disconnected_cb.callback = callback;
	_client_disconnected_cb.user_data = user_data;
}

void client_destroy_info(gpointer p)
{
	pims_ipc_client_info_s *client_info = p;

	if (NULL == client_info)
		return;
	free(client_info->smack);
	free(client_info->uid);
	free(client_info->client_session);
	free(client_info);
}

void client_init(void)
{
	unique_sequence_number = 0;
	worker_client_info_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
			client_destroy_info);
}

void client_deinit(void)
{
	DBG("----------destroied");
	g_hash_table_destroy(worker_client_info_map);
}

static int _create_client_info(int fd, pims_ipc_client_info_s **p_client_info)
{
	int ret;
	pid_t pid;
	char errmsg[1024] = {0};

	pims_ipc_client_info_s *client_info = calloc(1, sizeof(pims_ipc_client_info_s));
	if (NULL == client_info) {
		ERR("calloc() return NULL");
		return -1;
	}

	ret = cynara_creds_socket_get_client(fd, CLIENT_METHOD_SMACK, &(client_info->smack));
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERR("cynara_creds_socket_get_client() Fail(%d,%s)", ret, errmsg);
		client_destroy_info(client_info);
		return -1;
	}

	ret = cynara_creds_socket_get_user(fd, USER_METHOD_UID, &(client_info->uid));
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERR("cynara_creds_socket_get_user() Fail(%d,%s)", ret, errmsg);
		client_destroy_info(client_info);
		return -1;
	}

	ret = cynara_creds_socket_get_pid(fd, &pid);
	if (CYNARA_API_SUCCESS != ret) {
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERR("cynara_creds_socket_get_pid() Fail(%d,%s)", ret, errmsg);
		client_destroy_info(client_info);
		return -1;
	}

	client_info->client_session = cynara_session_from_pid(pid);
	if (NULL == client_info->client_session) {
		ERR("cynara_session_from_pid() return NULL");
		client_destroy_info(client_info);
		return -1;
	}
	*p_client_info = client_info;

	return 0;
}

int client_register_info(int client_fd, int client_pid)
{
	pims_ipc_client_info_s *client_info = NULL;
	int ret = 0;

	ret = _create_client_info(client_fd, &client_info);
	if (ret < 0) {
		ERR("_create_client_info() Fail(%d)", ret);
		return -1;
	}

	g_hash_table_insert(worker_client_info_map, GINT_TO_POINTER(client_pid), client_info);
	DBG("-------inserted:pid(%d), info(%p)", client_pid, client_info);

	return 0;
}

int client_get_unique_sequence_number(void)
{
	return unique_sequence_number++;
}

pims_ipc_client_info_s* client_clone_info(pims_ipc_client_info_s *client_info)
{
	if (NULL == client_info) {
		ERR("client_info is NULL");
		return NULL;
	}

	pims_ipc_client_info_s *clone = calloc(1, sizeof(pims_ipc_client_info_s));
	if (NULL == clone) {
		ERR("calloc() Fail");
		return NULL;
	}

	if (client_info->smack) {
		clone->smack = strdup(client_info->smack);
		if (NULL == clone->smack) {
			ERR("strdup() Fail");
			client_destroy_info(clone);
			return NULL;
		}
	}

	if (client_info->uid) {
		clone->uid = strdup(client_info->uid);
		if (NULL == clone->uid) {
			ERR("strdup() Fail");
			client_destroy_info(clone);
			return NULL;
		}
	}

	if (client_info->client_session) {
		clone->client_session = strdup(client_info->client_session);
		if (NULL == clone->client_session) {
			ERR("strdup() Fail");
			client_destroy_info(clone);
			return NULL;
		}
	}

	return clone;
}

pims_ipc_client_info_s* client_get_info(int client_pid)
{
	pims_ipc_client_info_s *client_info = NULL;

	client_info = g_hash_table_lookup(worker_client_info_map, GINT_TO_POINTER(client_pid));
	DBG("---------------get client_pid(%d)", client_pid);
	return client_info;
}

