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


#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "pims-internal.h"
#include "pims-debug.h"
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
	//	ENTER();
	int ret = -1;
	pims_ipc_data_s *handle = NULL;

	handle = calloc(1, sizeof(pims_ipc_data_s));
	if (NULL == handle) {
		ERROR("calloc() Fail");
		return NULL;
	}
	handle->alloc_size = size;
	handle->free_size = size;
	handle->buf_size = 0;
	handle->buf = calloc(1, size);
	if (NULL == handle->buf) {
		ERROR("calloc() Fail");
		free(handle);
		return NULL;
	}
	handle->pos = handle->buf;
	handle->created = 1;
	handle->buf_alloced = 1;

	ret = pims_ipc_data_put(handle, &flags, sizeof(int));

	ASSERT(ret == 0);
	handle->flags = flags;

	return handle;
}

API void pims_ipc_data_destroy(pims_ipc_data_h data)
{
	//	ENTER();
	pims_ipc_data_s *handle = (pims_ipc_data_s *)data;
	if (!handle)
		return;

	if (handle->buf_alloced)
		free(handle->buf);

	free(handle);
}

API int pims_ipc_data_put(pims_ipc_data_h data, void *buf, unsigned int size)
{
	//	ENTER();
	pims_ipc_data_s *handle = NULL;
	unsigned int dsize = size;
	unsigned int used_size = 0;
	handle = (pims_ipc_data_s *)data;

	if (handle->created == 0) {
		ERROR("This handle isn't create mode.");
		return -1;
	}

	if (handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE) {
		ERROR("Not without-type mode");
		return -1;
	}

	if (dsize > 0 && buf == NULL) {
		ERROR("Invalid argument");
		return -1;
	}

	used_size = _get_used_size(dsize);
	if (handle->free_size < used_size) {
		int new_size = 0;
		new_size = handle->alloc_size * 2;

		while (new_size < handle->buf_size + used_size)
			new_size *= 2;
		handle->buf = realloc(handle->buf, new_size);
		if (NULL == handle->buf) {
			ERROR("realloc() Fail");
			return -1;
		}
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
	//	ENTER();
	pims_ipc_data_s *handle = NULL;
	unsigned int dsize = 0;
	unsigned int used_size = 0;
	void *buf = NULL;
	handle = (pims_ipc_data_s *)data;

	if (handle->created == 1) {
		ERROR("This handle is create mode.");
		return NULL;
	}
	if (handle->buf_size == 0) {
		ERROR("Remain buffer size is 0.");
		return NULL;
	}
	if (handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE) {
		ERROR("Not without-type mode");
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
	//	ENTER();
	void *ptr = NULL;
	pims_ipc_data_s *handle = NULL;

	VERBOSE("size : %d", size);
	handle = calloc(1, sizeof(pims_ipc_data_s));
	if (NULL == handle) {
		ERROR("calloc() Fail");
		return NULL;
	}
	handle->alloc_size = size;
	handle->free_size = 0;
	handle->buf_size = handle->alloc_size;
	handle->buf = buf;
	handle->pos = handle->buf;
	handle->created = 0;
	handle->buf_alloced = 1;

	ptr = pims_ipc_data_get(handle, &size);
	if (!ptr || size != sizeof(int)) {
		ERROR("pims_ipc_data_get : error");
		free(handle->buf);
		free(handle);
		return NULL;
	}
	handle->flags = *((int*)ptr);

	VERBOSE("handle[%p] flags[%x]", handle, handle->flags);

	return handle;
}

