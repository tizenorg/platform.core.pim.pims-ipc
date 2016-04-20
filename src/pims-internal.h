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

#ifndef __PIMS_INTERNAL_H__
#define __PIMS_INTERNAL_H__

#include <glib.h>

#include "pims-debug.h"


#ifdef __cplusplus
extern "C"
{
#endif

#ifndef API
#define API __attribute__ ((visibility("default")))
#endif

#define MAX_EPOLL_EVENT 256

#define PIMS_IPC_MODULE_INTERNAL    "pims_ipc_internal"
#define PIMS_IPC_FUNCTION_CREATE    "create"
#define PIMS_IPC_FUNCTION_DESTROY   "destroy"
#define PIMS_IPC_CALL_ID_CREATE     PIMS_IPC_MODULE_INTERNAL ":" PIMS_IPC_FUNCTION_CREATE
#define PIMS_IPC_CALL_ID_DESTROY    PIMS_IPC_MODULE_INTERNAL ":" PIMS_IPC_FUNCTION_DESTROY
#define PIMS_IPC_MAKE_CALL_ID(module, function) g_strdup_printf("%s:%s", module, function)

typedef struct {
	unsigned int alloc_size;
	unsigned int buf_size;
	unsigned int free_size;
	char *pos;
	char *buf;
	unsigned int created:1;
	unsigned int buf_alloced:1;
} pims_ipc_data_s;

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_INTERNAL_H__ */
