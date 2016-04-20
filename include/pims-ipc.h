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


#ifndef __PIMS_IPC_H__
#define __PIMS_IPC_H__

#include <pims-ipc-types.h>

#ifdef __cplusplus
extern "C"
{
#endif

pims_ipc_h pims_ipc_create(char *service);
void pims_ipc_destroy(pims_ipc_h ipc);
int pims_ipc_call(pims_ipc_h ipc, char *module, char *function, pims_ipc_data_h data_in,
		pims_ipc_data_h *data_out);
int pims_ipc_call_async(pims_ipc_h ipc, char *module, char *function,
		pims_ipc_data_h data_in, pims_ipc_call_async_cb callback, void *user_data);
int pims_ipc_is_call_in_progress(pims_ipc_h ipc);

pims_ipc_h pims_ipc_create_for_subscribe(char *service);
void pims_ipc_destroy_for_subscribe(pims_ipc_h ipc);
int pims_ipc_subscribe(pims_ipc_h ipc, char *module, char *event,
		pims_ipc_subscribe_cb callback, void *user_data);
int pims_ipc_unsubscribe(pims_ipc_h ipc, char *module, char *event);
int pims_ipc_add_server_disconnected_cb(pims_ipc_h ipc,
		pims_ipc_server_disconnected_cb callback, void *user_data);
int pims_ipc_remove_server_disconnected_cb(pims_ipc_h ipc);

/* start deprecated */
int pims_ipc_set_server_disconnected_cb(pims_ipc_server_disconnected_cb callback, void *user_data); /* use pims_ipc_add_server_disconnected_cb */
int pims_ipc_unset_server_disconnected_cb(); /* use pims_ipc_remove_server_disconnected_cb */
/* end deprecated */


#ifdef __cplusplus
}
#endif

#endif /*__PIMS_IPC_H__*/

