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


#ifndef __PIMS_SOCKET_H__
#define __PIMS_SOCKET_H__

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _cplusplus
extern "C"
{
#endif

typedef void (*server_socket_client_closed_cb)(const char *pid, void *user_data);
int _server_socket_init(const char *path, gid_t group, mode_t mode,
        server_socket_client_closed_cb callback, void *user_data);
int _client_socket_init(const char *path, const char *pid);

#ifdef _cplusplus
}
#endif

#endif /* __PIMS_SOCKET_H__ */

