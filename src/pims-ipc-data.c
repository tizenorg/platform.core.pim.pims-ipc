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


#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "pims-internal.h"
#include "pims-ipc-data.h"

/*
   Structure of data with type(4 bytes order)
   +------------------------------------------------------------------+
   | type | size | data        | pad | type | size | data       | pad |
   +------------------------------------------------------------------+
   4      4     size         0-3          (Size of bytes)

   Structure of data without type(4 bytes order)
   +----------------------------------------------------+
   | size | data        | pad | size | data       | pad |
   +----------------------------------------------------+
   4     size         0-3        (Size of bytes)
   */

#define _get_used_size_with_type(data_size) \
	(sizeof(int) + sizeof(int) + data_size + ((sizeof(int) - (data_size % sizeof(int)))%sizeof(int)))

#define _get_used_size(data_size) \
	(sizeof(int) + data_size + ((sizeof(int) - (data_size % sizeof(int)))%sizeof(int)))

API pims_ipc_data_h pims_ipc_data_create_with_size(unsigned int size, int flags)
{
	//	FN_CALL();
	int ret = -1;
	pims_ipc_data_s *handle = NULL;

	handle = calloc(1, sizeof(pims_ipc_data_s));
	if (NULL == handle) {
		ERR("calloc() Fail");
		return NULL;
	}

	if (flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE) {
		ERR("Not supported with-type mode");
		free(handle);
		return NULL;
	}

	handle->alloc_size = size;
	handle->free_size = size;
	handle->buf_size = 0;
	handle->buf = calloc(1, size);
	handle->pos = handle->buf;
	handle->created = 1;
	handle->buf_alloced = 1;

	return handle;
}

API void pims_ipc_data_destroy(pims_ipc_data_h data)
{
	//	FN_CALL();
	pims_ipc_data_s *handle = data;
	if (NULL == handle)
		return;

	if (handle->buf_alloced)
		free(handle->buf);

	free(handle);
}

API int pims_ipc_data_put(pims_ipc_data_h data, void *buf, unsigned int size)
{
	//	FN_CALL();
	pims_ipc_data_s *handle = data;
	unsigned int dsize = size;
	unsigned int used_size = 0;

	if (handle->created == 0) {
		ERR("This handle isn't create mode.");
		return -1;
	}

	if (dsize > 0 && buf == NULL) {
		ERR("Invalid argument");
		return -1;
	}

	used_size = _get_used_size(dsize);
	if (handle->free_size < used_size) {
		int new_size = 0;
		new_size = handle->alloc_size * 2;

		while (new_size < handle->buf_size + used_size)
			new_size *= 2;
		handle->buf = realloc(handle->buf, new_size);
		handle->alloc_size = new_size;
		handle->free_size = handle->alloc_size - handle->buf_size;
		handle->pos = handle->buf;
		handle->pos += handle->buf_size;
		VERBOSE("free_size [%d] dsize [%d]", handle->free_size, dsize);
	}

	*(int*)(handle->pos) = dsize;
	if (dsize > 0) {
		memcpy((handle->pos+sizeof(int)), buf, dsize);
		int pad_size = used_size-dsize-sizeof(int);
		if (pad_size > 0)
			memset((handle->pos+sizeof(int)+dsize), 0x0, pad_size);
	}

	handle->pos += used_size;
	handle->buf_size += used_size;
	handle->free_size -= used_size;

	VERBOSE("data put size [%u] data[%p]", dsize, buf);

	return 0;
}

API void* pims_ipc_data_get(pims_ipc_data_h data, unsigned int *size)
{
	//	FN_CALL();
	pims_ipc_data_s *handle = data;
	unsigned int dsize = 0;
	unsigned int used_size = 0;
	void *buf = NULL;

	if (handle->created == 1) {
		ERR("This handle is create mode.");
		return NULL;
	}
	if (handle->buf_size == 0) {
		ERR("Remain buffer size is 0.");
		return NULL;
	}

	dsize = *(int*)(handle->pos);
	buf = (handle->pos+sizeof(int));

	if (dsize == 0) // current position is next size field becasue data size is 0
		buf = NULL;

	used_size = _get_used_size(dsize);
	handle->pos += used_size;
	handle->buf_size -= used_size;
	handle->free_size += used_size;

	VERBOSE("data get size [%u] data[%p]", dsize, buf);
	*size = dsize;
	return buf;
}

pims_ipc_data_h pims_ipc_data_steal_unmarshal(void *buf, unsigned int size)
{
	//	FN_CALL();
	void *ptr = NULL;
	pims_ipc_data_s *handle = NULL;

	VERBOSE("size : %d", size);
	handle = calloc(1, sizeof(pims_ipc_data_s));
	if (NULL == handle) {
		ERR("calloc() handle Fail");
		return NULL;
	}
	handle->buf = calloc(1, size+1);
	if (NULL == handle->buf ) {
		ERR("calloc() buf Fail");
		free(handle);
		return NULL;
	}
	memcpy(handle->buf, buf, size);

	handle->alloc_size = size;
	handle->free_size = 0;
	handle->buf_size = handle->alloc_size;
	handle->pos = handle->buf;
	handle->created = 0;
	handle->buf_alloced = 1;

	VERBOSE("handle[%p]", handle);

	return handle;
}

