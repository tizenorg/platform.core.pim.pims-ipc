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

#include <pims-internal.h>
#include <pims-debug.h>
#include <pims-ipc-data.h>

typedef struct
{
    unsigned int alloc_size;
    unsigned int buf_size;
    unsigned int free_size;
    zmq_msg_t zmsg;
    char *pos;
    char *buf;
    int flags;
    unsigned int created:1;
    unsigned int zmsg_alloced:1;
    unsigned int buf_alloced:1;
} pims_ipc_data_t;

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
    (sizeof(int) + sizeof(int) + data_size + (sizeof(int) - (data_size % sizeof(int))))

#define _get_used_size(data_size) \
    (sizeof(int) + data_size + (sizeof(int) - (data_size % sizeof(int))))

API pims_ipc_data_h pims_ipc_data_create_with_size(unsigned int size, int flags)
{
    int ret = -1;
    pims_ipc_data_t *handle = NULL;

    handle = g_new0(pims_ipc_data_t, 1);
    handle->alloc_size = size;
    handle->free_size = size;
    handle->buf_size = 0;
    handle->buf = g_malloc0(size);
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
    pims_ipc_data_t *handle = (pims_ipc_data_t *)data;
    if (handle->buf_alloced)
    {
        g_free(handle->buf);
    }
    if (handle->zmsg_alloced)
    {
        zmq_msg_close(&handle->zmsg);
    }
    g_free(handle);
}

API int pims_ipc_data_put(pims_ipc_data_h data, void *buf, unsigned int size)
{
    pims_ipc_data_t *handle = NULL;
    unsigned int dsize = size;
    unsigned int used_size = 0;
    handle = (pims_ipc_data_t *)data;

    if (handle->created == 0)
    {
        ERROR("This handle isn't create mode.");
        return -1;
    }
    if (handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE)
    {
        ERROR("Not without-type mode");
        return -1;
    }
    if (dsize > 0 && buf == NULL)
    {
        ERROR("Invalid argument");
        return -1;
    }

    if (handle->free_size < _get_used_size(dsize))
    {
        int new_size = 0;
        new_size = handle->alloc_size * 2;

        while (new_size < handle->buf_size + _get_used_size(dsize))
            new_size *= 2;
        handle->buf = g_realloc(handle->buf, new_size);
        handle->alloc_size = new_size;
        handle->free_size = handle->alloc_size - handle->buf_size;

        handle->pos = handle->buf;
        handle->pos += handle->buf_size;

        VERBOSE("free_size [%d] dsize [%d]", handle->free_size, dsize);
    }

    *(int*)(handle->pos) = dsize;
    if (dsize > 0)
        memcpy((handle->pos+sizeof(int)), buf, dsize);

    used_size = _get_used_size(dsize);
    handle->pos += used_size;
    handle->buf_size += used_size;
    handle->free_size -= used_size;
    
    VERBOSE("data put size [%u] data[%p]", dsize, buf);

    return 0;
}

API void* pims_ipc_data_get(pims_ipc_data_h data, unsigned int *size)
{
    pims_ipc_data_t *handle = NULL;
    unsigned int dsize = 0;
    unsigned int used_size = 0;
    void *buf = NULL;
    handle = (pims_ipc_data_t *)data;

    if (handle->created == 1)
    {
        ERROR("This handle is create mode.");
        return NULL;
    }
    if (handle->buf_size == 0)
    {
        ERROR("Remain buffer size is 0.");
        return NULL;
    }
    if (handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE)
    {
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

API void* pims_ipc_data_get_dup(pims_ipc_data_h data, unsigned int *size)
{
    void *buf = NULL;

    buf = pims_ipc_data_get(data, size);
    return g_memdup(buf, *size);
}


API int pims_ipc_data_put_with_type(pims_ipc_data_h data, pims_ipc_data_type_e type, void *buf, unsigned int size)
{
    pims_ipc_data_t *handle = NULL;
    unsigned int dsize = 0;
    unsigned int used_size = 0;
    handle = (pims_ipc_data_t *)data;

    if (handle->created == 0)
    {
        ERROR("This handle isn't create mode.");
        return -1;
    }
    if (!(handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE))
    {
        ERROR("Not with-type mode");
        return -1;
    }

    switch(type)
    {
    case PIMS_IPC_DATA_TYPE_CHAR:
        dsize = sizeof(char);
        break;
    case PIMS_IPC_DATA_TYPE_UCHAR:
        dsize = sizeof(unsigned char);
        break;
    case PIMS_IPC_DATA_TYPE_INT:
        dsize = sizeof(int);
        break;
    case PIMS_IPC_DATA_TYPE_UINT:
        dsize = sizeof(unsigned int);
        break;
    case PIMS_IPC_DATA_TYPE_LONG:
        dsize = sizeof(long);
        break;
    case PIMS_IPC_DATA_TYPE_ULONG:
        dsize = sizeof(unsigned long);
        break;
    case PIMS_IPC_DATA_TYPE_FLOAT:
        dsize = sizeof(float);
        break;
    case PIMS_IPC_DATA_TYPE_DOUBLE:
        dsize = sizeof(double);
        break;
    case PIMS_IPC_DATA_TYPE_STRING:
        if (buf == NULL)
        {
            dsize = 0;
        }
        else
        {
            dsize = strlen(buf) +1;
        }
        break;
    case PIMS_IPC_DATA_TYPE_MEMORY:
        dsize = size;
        break;
    default:
        dsize = 0;
        break;
    }

    if (dsize != 0 && buf == NULL)
        return -1;

    if (buf == NULL && dsize == 0 && type != PIMS_IPC_DATA_TYPE_STRING)
        return -1;

    if (handle->free_size < _get_used_size_with_type(dsize))
    {
        int new_size = 0;
        new_size = handle->alloc_size * 2;

        while (new_size < handle->buf_size + _get_used_size_with_type(dsize))
            new_size *= 2;
        handle->buf = g_realloc(handle->buf, new_size);
        handle->alloc_size = new_size;
        handle->free_size = handle->alloc_size - handle->buf_size;

        handle->pos = handle->buf;
        handle->pos += handle->buf_size;

        VERBOSE("free_size [%d] dsize [%d]", handle->free_size, dsize);
    }

    *(int*)(handle->pos) = type;
    *(int*)(handle->pos+sizeof(int)) = dsize;
    if (dsize > 0 && buf != NULL)
        memcpy((handle->pos+sizeof(int)*2), buf, dsize);

    used_size = _get_used_size_with_type(dsize);
    handle->pos += used_size;
    handle->buf_size += used_size;
    handle->free_size -= used_size;
    
    VERBOSE("data put type [%d] size [%u] data[%p]", type, dsize, buf);

    return 0;
}

API void* pims_ipc_data_get_with_type(pims_ipc_data_h data, pims_ipc_data_type_e *type, unsigned int *size)
{
    pims_ipc_data_t *handle = NULL;
    pims_ipc_data_type_e dtype = PIMS_IPC_DATA_TYPE_INVALID;
    unsigned int dsize = 0;
    unsigned int used_size = 0;
    void *buf = NULL;
    handle = (pims_ipc_data_t *)data;

    if (handle->created == 1)
    {
        ERROR("This handle is create mode.");
        *type = PIMS_IPC_DATA_TYPE_INVALID;
        return NULL;
    }
    if (handle->buf_size == 0)
    {
        ERROR("Remain buffer size is 0.");
        *type = PIMS_IPC_DATA_TYPE_INVALID;
        return NULL;
    }
    if (!(handle->flags & PIMS_IPC_DATA_FLAGS_WITH_TYPE))
    {
        ERROR("Not with-type mode");
        *type = PIMS_IPC_DATA_TYPE_INVALID;
        return NULL;
    }

    dtype = *(int*)(handle->pos);
    dsize = *(int*)(handle->pos+sizeof(int));
    buf = (handle->pos+sizeof(int)*2);

    switch(dtype)
    {
        case PIMS_IPC_DATA_TYPE_CHAR:
        case PIMS_IPC_DATA_TYPE_UCHAR:
        case PIMS_IPC_DATA_TYPE_INT:
        case PIMS_IPC_DATA_TYPE_UINT:
        case PIMS_IPC_DATA_TYPE_LONG:
        case PIMS_IPC_DATA_TYPE_ULONG:
        case PIMS_IPC_DATA_TYPE_FLOAT:
        case PIMS_IPC_DATA_TYPE_DOUBLE:
        case PIMS_IPC_DATA_TYPE_STRING:
        case PIMS_IPC_DATA_TYPE_MEMORY:
            break;
        default:
            ERROR("Unknown data type");
            dsize = 0;
            break;
    }

    if (dtype != PIMS_IPC_DATA_TYPE_STRING && dsize == 0)
    {
        *type = PIMS_IPC_DATA_TYPE_INVALID;
        return NULL;
    }

    if (dsize == 0) // current position is next type field becasue data size is 0
        buf = NULL;

    used_size = _get_used_size_with_type(dsize);
    handle->pos += used_size;
    handle->buf_size -= used_size;
    handle->free_size += used_size;

    VERBOSE("data get type [%d] size [%u] data[%p]", dtype, dsize, buf);
    *type = dtype;
    *size = dsize;
    return buf;
}

API void* pims_ipc_data_get_dup_with_type(pims_ipc_data_h data, pims_ipc_data_type_e *type, unsigned int *size)
{
    void *buf = NULL;

    buf = pims_ipc_data_get_with_type(data, type, size);
    return g_memdup(buf, *size);
}

API void* pims_ipc_data_marshal(pims_ipc_data_h data, unsigned int *size)
{
    pims_ipc_data_t *handle = NULL;
    
    if (!data || !size )
        return NULL;
    
    handle = (pims_ipc_data_t *)data;

    *size = handle->buf_size;

    return handle->buf;
}

static void __pims_ipc_data_free_cb(void *data, void *hint)
{
    if (hint)
        g_free(hint);
}

API int pims_ipc_data_marshal_with_zmq(pims_ipc_data_h data, zmq_msg_t *pzmsg)
{
    pims_ipc_data_t *handle = NULL;
   
    if (!data || !pzmsg )
        return -1;

    handle = (pims_ipc_data_t *)data;

    if (handle->zmsg_alloced)
    {
        // TODO: need to prevent memory leackage when reusing data marshaled
        WARNING("It's ever been marshaled");
    }

    zmq_msg_init_data(&handle->zmsg, handle->buf, handle->buf_size, __pims_ipc_data_free_cb, handle->buf); 
    
    if (zmq_msg_copy(pzmsg, &handle->zmsg) != 0)
    {
        zmq_msg_close(&handle->zmsg);
        return -1;
    }

    handle->zmsg_alloced = 1;
    handle->buf_alloced = 0;

    return 0;
}

API void* pims_ipc_data_marshal_dup(pims_ipc_data_h data, unsigned int *size)
{
    unsigned int lsize = 0;
    gpointer buf = NULL;
    
    if (!data || !size )
        return NULL;
    
    buf = pims_ipc_data_marshal(data, &lsize);
    *size = lsize;
    return g_memdup(buf, lsize);
}

API pims_ipc_data_h pims_ipc_data_unmarshal(void *buf, unsigned int size)
{
    pims_ipc_data_t *handle = NULL;
    zmq_msg_t zmsg; 

    zmq_msg_init_data(&zmsg, buf, size, __pims_ipc_data_free_cb, NULL); 

    handle = pims_ipc_data_unmarshal_with_zmq(&zmsg);
    zmq_msg_close(&zmsg);

    return handle;
}

API pims_ipc_data_h pims_ipc_data_unmarshal_with_zmq(zmq_msg_t *pzmsg)
{
    pims_ipc_data_t *handle = NULL;
    unsigned int size = 0;
    void *ptr = NULL;

    handle = g_new0(pims_ipc_data_t, 1);
    zmq_msg_init(&handle->zmsg);
    if (zmq_msg_copy(&handle->zmsg, pzmsg) != 0)
    {
        g_free(handle);
        return NULL;
    }
    handle->alloc_size = zmq_msg_size(&handle->zmsg);
    handle->free_size = 0;
    handle->buf_size = handle->alloc_size;
    handle->buf = zmq_msg_data(&handle->zmsg);
    handle->pos = handle->buf;
    handle->created = 0;
    handle->zmsg_alloced = 1;
    
    ptr = pims_ipc_data_get(handle, &size);
    if (!ptr || size != sizeof(int))
    {
        g_free(handle);
        return NULL;
    }
    handle->flags = *((int*)ptr);

    VERBOSE("handle[%p] zmsg[%p] flags[%x]", handle, pzmsg, handle->flags);
    
    return handle;
}

API pims_ipc_data_h pims_ipc_data_unmarshal_dup(void *buf, unsigned int size)
{
    pims_ipc_data_t *handle = NULL;
    zmq_msg_t zmsg; 

    zmq_msg_init_size(&zmsg, size);
    memcpy(zmq_msg_data(&zmsg), buf, size);

    handle = pims_ipc_data_unmarshal_with_zmq(&zmsg);
    zmq_msg_close(&zmsg);

    return handle;
}
