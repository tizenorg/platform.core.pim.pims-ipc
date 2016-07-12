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

#ifndef __PIMS_IPC_DATA_INTERNAL_H__
#define __PIMS_IPC_DATA_INTERNAL_H__

#include <pthread.h>

#include <pims-ipc-types.h>
#include <cynara-client.h>
#include <cynara-session.h>
#include <cynara-creds-socket.h>

#ifdef __cplusplus
extern "C"
{
#endif

pims_ipc_data_h pims_ipc_data_steal_unmarshal(void *buf, unsigned int size);


typedef struct {
	char *service;
	gid_t group;
	mode_t mode;

	/* Global socket info and epoll thread */
	int sockfd;

	GHashTable *client_worker_map; /* key : client_id, value: worker_data */
	GList *client_id_fd_map; /* pims_ipc_client_map_s = client_id:client_fd */

	int workers_max_count;

	pthread_mutex_t manager_list_hangup_mutex;
	GList *manager_list_hangup;

	cynara *cynara;
	pthread_mutex_t cynara_mutex;
} pims_ipc_svc_s;

typedef struct {
	char *client_id;
	unsigned int client_id_len;
	unsigned int seq_no;
	char *call_id;
	unsigned int call_id_len;
	unsigned int has_data;
	unsigned int data_len;
	char *data;
} pims_ipc_raw_data_s;

typedef struct {
	int fd;
	int client_fd;
	int stop_thread;
	int client_pid;
	GList *list;		/* pims_ipc_raw_data_s list */
	pthread_mutex_t queue_mutex;
	pthread_mutex_t ready_mutex;
	pthread_cond_t ready;
} pims_ipc_worker_data_s;

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_IPC_DATA_INTERNAL_H__ */

