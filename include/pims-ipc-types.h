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


#ifndef __PIMS_IPC_TYPES_H__
#define __PIMS_IPC_TYPES_H__

#include <glib.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C"
{
#endif

#define PIMS_IPC_DATA_FLAGS_NONE        0x00000000
#define PIMS_IPC_DATA_FLAGS_WITH_TYPE   0x00000001

typedef void* pims_ipc_h;
typedef void* pims_ipc_data_h;

typedef enum {
    PIMS_IPC_DATA_TYPE_INVALID,
    PIMS_IPC_DATA_TYPE_CHAR,
    PIMS_IPC_DATA_TYPE_UCHAR,
    PIMS_IPC_DATA_TYPE_INT,
    PIMS_IPC_DATA_TYPE_UINT,
    PIMS_IPC_DATA_TYPE_LONG,
    PIMS_IPC_DATA_TYPE_ULONG,
    PIMS_IPC_DATA_TYPE_FLOAT,
    PIMS_IPC_DATA_TYPE_DOUBLE,
    PIMS_IPC_DATA_TYPE_STRING,
    PIMS_IPC_DATA_TYPE_MEMORY,
} pims_ipc_data_type_e;

typedef void (*pims_ipc_svc_call_cb)(pims_ipc_h ipc, pims_ipc_data_h data_in,
                                    pims_ipc_data_h *data_out, void *userdata);
typedef void (*pims_ipc_svc_client_disconnected_cb)(pims_ipc_h ipc, void *userdata);

typedef void (*pims_ipc_call_async_cb)(pims_ipc_h ipc, pims_ipc_data_h data_out, void *userdata);
typedef void (*pims_ipc_subscribe_cb)(pims_ipc_h ipc, pims_ipc_data_h data, void *userdata);


#ifdef __cplusplus
}
#endif

#endif /* __PIMS_IPC_TYPES_H__ */

