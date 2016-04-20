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

#ifndef __PIMS_SOCKET_H__
#define __PIMS_SOCKET_H__

#include <stdint.h>

#include "pims-ipc-data-internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct {
	int client_fd;
	int request_count;
	GList *raw_data; /* pims_ipc_raw_data_s list */
	pthread_mutex_t raw_data_mutex;
} pims_ipc_request_s;

typedef struct {
	int fd;
	char *id;
} pims_ipc_client_map_s;

int socket_send(int fd, char *buf, int len);
int socket_recv(int fd, void **buf, unsigned int len);
int socket_send_data(int fd, char *buf, unsigned int len);

int write_command(int fd, const uint64_t cmd);
int read_command(int fd, uint64_t *cmd);

void socket_remove_client_fd_map(pims_ipc_svc_s *ipc_svc);
void socket_set_handler(void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_SOCKET_H__ */

