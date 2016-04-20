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
#include <stdlib.h>
#include <stdint.h>
#include <poll.h>				// pollfds
#include <fcntl.h>				//fcntl
#include <unistd.h>
#include <error.h>
#include <sys/eventfd.h>	// eventfd

#include <glib.h>

#include "pims-internal.h"
#include "pims-socket.h"
#include "pims-ipc-data.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc-utils.h"
#include "pims-ipc-pubsub.h"
#include "pims-ipc-worker.h"
#include "pims-ipc-svc.h"

#define PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT  2

static pims_ipc_svc_s *_g_singleton = NULL;

gboolean svc_map_client_worker(gpointer user_data)
{
	char *client_id = user_data;
	pims_ipc_worker_data_s *worker_data;

	worker_data = worker_get_idle_worker(_g_singleton, client_id);
	if (NULL == worker_data) {
		ERR("worker_get_idle_worker() Fail");
		return G_SOURCE_CONTINUE;
	}
	return G_SOURCE_REMOVE;
}

API int pims_ipc_svc_init(char *service, gid_t group, mode_t mode)
{
	RETVM_IF(_g_singleton, -1, "Already exist");

	int ret = 0;

	_g_singleton = g_new0(pims_ipc_svc_s, 1);
	if (NULL == _g_singleton) {
		ERR("g_new0() Fail");
		return -1;
	}

	ret = cynara_initialize(&_g_singleton->cynara, NULL);
	if (CYNARA_API_SUCCESS != ret) {
		char errmsg[1024] = {0};
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERR("cynara_initialize() Fail(%d,%s)", ret, errmsg);
		return -1;
	}

	_g_singleton->service = g_strdup(service);
	_g_singleton->group = group;
	_g_singleton->mode = mode;
	_g_singleton->workers_max_count = PIMS_IPC_WORKERS_DEFAULT_MAX_COUNT;

	_g_singleton->client_worker_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);		// client id - worker_fd mapping
	ASSERT(_g_singleton->client_worker_map);

	pthread_mutex_init(&_g_singleton->manager_list_hangup_mutex, 0);
	_g_singleton->manager_list_hangup = NULL;

	worker_init();
	client_init();

	pthread_mutex_init(&_g_singleton->cynara_mutex, 0);
	return 0;
}

static void __remove_client_fd_map(pims_ipc_svc_s *ipc_svc)
{
	GList *next, *cursor;
	pims_ipc_client_map_s *client;

	cursor = g_list_first(ipc_svc->client_id_fd_map);
	while(cursor) {
		next = cursor->next;
		client = cursor->data;
		worker_stop_client_worker(ipc_svc, client->id);
		free(client->id);
		free(client);
		cursor = next;
	}
	g_list_free(ipc_svc->client_id_fd_map);
}

API int pims_ipc_svc_deinit(void)
{
	RETV_IF(NULL == _g_singleton, -1);

	g_free(_g_singleton->service);

	pthread_mutex_destroy(&_g_singleton->manager_list_hangup_mutex);
	g_list_free_full(_g_singleton->manager_list_hangup, g_free);

	__remove_client_fd_map(_g_singleton);
	worker_deinit();
	g_hash_table_destroy(_g_singleton->client_worker_map);
	client_deinit();

	pthread_mutex_lock(&_g_singleton->cynara_mutex);
	int ret = cynara_finish(_g_singleton->cynara);
	if (CYNARA_API_SUCCESS != ret) {
		char errmsg[1024] = {0};
		cynara_strerror(ret, errmsg, sizeof(errmsg));
		ERR("cynara_finish() Fail(%d,%s)", ret, errmsg);
	}
	pthread_mutex_unlock(&_g_singleton->cynara_mutex);
	pthread_mutex_destroy(&_g_singleton->cynara_mutex);

	g_free(_g_singleton);
	_g_singleton = NULL;

	return 0;
}

API int pims_ipc_svc_register(char *module, char *function,
		pims_ipc_svc_call_cb callback, void *userdata)
{
	gchar *call_id = NULL;
	pims_ipc_svc_cb_s *cb_data = NULL;

	RETV_IF(NULL == module, -1);
	RETV_IF(NULL == function, -1);
	RETV_IF(NULL == callback, -1);

	cb_data = calloc(1, sizeof(pims_ipc_svc_cb_s));
	if (NULL == cb_data) {
		ERR("calloc() Fail");
		return -1;
	}

	call_id = PIMS_IPC_MAKE_CALL_ID(module, function);
	VERBOSE("register cb id[%s]", call_id);

	cb_data->callback = callback;
	cb_data->user_data = userdata;

	worker_set_callback(call_id, cb_data);

	return 0;
}

API void pims_ipc_svc_run_main_loop(GMainLoop *loop)
{
	GMainLoop *main_loop = loop;

	if (main_loop == NULL)
		main_loop = g_main_loop_new(NULL, FALSE);

	pubsub_start();

	if (_g_singleton) {
		// launch worker threads in advance
		worker_start_idle_worker(_g_singleton);
		socket_set_handler(_g_singleton);
	}

	g_main_loop_run(main_loop);

	worker_stop_idle_worker();
	pubsub_stop();
}

API bool pims_ipc_svc_check_privilege(pims_ipc_h ipc, char *privilege)
{
	int ret;
	int pid = (int)ipc;

	RETV_IF(NULL == privilege, false);

	pims_ipc_client_info_s *client_info = NULL;
	client_info = client_get_info(pid);
	if (NULL == client_info) {
		ERR("client_info is NULL");
		return false;
	}
	pims_ipc_client_info_s *client_clone = NULL;
	client_clone = client_clone_info(client_info);

	pthread_mutex_lock(&_g_singleton->cynara_mutex);
	ret = cynara_check(_g_singleton->cynara, client_clone->smack,
			client_clone->client_session, client_clone->uid, privilege);
	pthread_mutex_unlock(&_g_singleton->cynara_mutex);

	client_destroy_info(client_clone);

	if (CYNARA_API_ACCESS_ALLOWED == ret)
		return true;

	return false;
}

API int pims_ipc_svc_get_smack_label(pims_ipc_h ipc, char **p_smack)
{
	pims_ipc_client_info_s *client_info = NULL;
	int pid = (int)ipc;

	client_info = client_get_info(pid);
	if (NULL == client_info) {
		ERR("client_info is NULL");
		return false;
	}

	if (client_info->smack) {
		*p_smack = strdup(client_info->smack);
		if (NULL == *p_smack) {
			ERR("strdup() return NULL");
			return -1;
		}
	}
	return 0;
}

