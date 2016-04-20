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
#ifndef __PIMS_WORKER_H__
#define __PIMS_WORKER_H__

#include "pims-ipc-data-internal.h"

typedef struct {
	pims_ipc_svc_call_cb callback;
	void *user_data;
} pims_ipc_svc_cb_s;

typedef struct {
	char *smack;
	char *uid;
	char *client_session;
} pims_ipc_client_info_s;

void worker_init();
void worker_deinit();
void worker_start_idle_worker(pims_ipc_svc_s *ipc_data);
void worker_stop_idle_worker();
void worker_stop_client_worker(pims_ipc_svc_s *ipc_svc, const char *client_id);


int worker_push_raw_data(pims_ipc_worker_data_s *worker_data, int client_fd,
		pims_ipc_raw_data_s *data);

int worker_wait_idle_worker_ready(pims_ipc_worker_data_s *worker_data);
pims_ipc_worker_data_s* worker_get_idle_worker(pims_ipc_svc_s *ipc_svc, const char *client_id);
int worker_set_callback(char *call_id, pims_ipc_svc_cb_s *cb_data);
pims_ipc_worker_data_s* worker_find(pims_ipc_svc_s *ipc_svc, const char *client_id);

void worker_free_data(gpointer data);
void worker_free_raw_data(void *data);

void client_init(void);
void client_deinit(void);
int client_register_info(int client_fd, int client_pid);
void client_destroy_info(gpointer p);
int client_get_unique_sequence_number(void);
pims_ipc_client_info_s* client_get_info(int worker_id);
pims_ipc_client_info_s* client_clone_info(pims_ipc_client_info_s *client_info);

#endif /*__PIMS_WORKER_H__*/


