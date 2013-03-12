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


#ifndef __PIMS_IPC_DATA_H__
#define __PIMS_IPC_DATA_H__

#include <pims-ipc-types.h>

#ifdef _cplusplus
extern "C"
{
#endif

#define pims_ipc_data_create(flags) pims_ipc_data_create_with_size(1024, (flags))

pims_ipc_data_h pims_ipc_data_create_with_size(unsigned int size, int flags);
void pims_ipc_data_destroy(pims_ipc_data_h ipc);
int pims_ipc_data_put(pims_ipc_data_h data, void *buf, unsigned int size);
void* pims_ipc_data_get(pims_ipc_data_h data, unsigned int *size);
void* pims_ipc_data_get_dup(pims_ipc_data_h data, unsigned int *size);
int pims_ipc_data_put_with_type(pims_ipc_data_h data, pims_ipc_data_type_e type, void *buf, unsigned int size);
void* pims_ipc_data_get_with_type(pims_ipc_data_h data, pims_ipc_data_type_e *type, unsigned int *size);
void* pims_ipc_data_get_dup_with_type(pims_ipc_data_h data, pims_ipc_data_type_e *type, unsigned int *size);

void* pims_ipc_data_marshal(pims_ipc_data_h data, unsigned int *size);
int pims_ipc_data_marshal_with_zmq(pims_ipc_data_h data, zmq_msg_t *pzmsg);
void* pims_ipc_data_marshal_dup(pims_ipc_data_h data, unsigned int *size);
pims_ipc_data_h pims_ipc_data_unmarshal(void *buf, unsigned int size);
pims_ipc_data_h pims_ipc_data_unmarshal_with_zmq(zmq_msg_t *pzmsg);
pims_ipc_data_h pims_ipc_data_unmarshal_dup(void *buf, unsigned int size);

#ifdef _cplusplus
}
#endif

#endif /* __PIMS_IPC_DATA_H__ */
