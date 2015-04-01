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


#ifndef __PIMS_IPC_SVC_H__
#define __PIMS_IPC_SVC_H__

#include <pims-ipc-types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C"
{
#endif

int pims_ipc_svc_init(char *service, gid_t group, mode_t mode);
int pims_ipc_svc_deinit(void);
int pims_ipc_svc_register(char *module, char *function, pims_ipc_svc_call_cb callback, void *userdata);

int pims_ipc_svc_init_for_publish(char *service, gid_t group, mode_t mode);
int pims_ipc_svc_deinit_for_publish(void);
int pims_ipc_svc_publish(char *module, char *event, pims_ipc_data_h data);

void pims_ipc_svc_run_main_loop(GMainLoop* main_loop);

void pims_ipc_svc_set_client_disconnected_cb(pims_ipc_svc_client_disconnected_cb callback, void *userdata);

pims_ipc_client_info_h pims_ipc_svc_find_client_info(pims_ipc_h ipc);
int pims_ipc_svc_create_client_info(int fd, pims_ipc_client_info_h *p_client_info);
int pims_ipc_svc_get_smack_label(pims_ipc_client_info_h client_info, char **p_smack);
bool pims_ipc_svc_check_privilege(pims_ipc_client_info_h client_info, char *privilege);
void pims_ipc_svc_destroy_client_info(pims_ipc_client_info_h client_info);


#ifdef __cplusplus
}
#endif

#endif /*__PIMS_IPC_IMPL_H__*/

