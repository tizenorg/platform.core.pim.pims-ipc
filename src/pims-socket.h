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


#ifndef __PIMS_SOCKET_H__
#define __PIMS_SOCKET_H__

#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

int socket_send(int fd, char *buf, int len);
int socket_recv(int fd, void **buf, unsigned int len);
int socket_send_data(int fd, char *buf, unsigned int len);

int write_command(int fd, const uint64_t cmd);
int read_command(int fd, uint64_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_SOCKET_H__ */

