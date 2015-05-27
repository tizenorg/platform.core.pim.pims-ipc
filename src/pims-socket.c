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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "pims-internal.h"
#include "pims-debug.h"
#include "pims-socket.h"

#define MAX_ARRAY_LEN	65535

int socket_send(int fd, char *buf, int len)
{
	if (!buf || len <= 0) {
		INFO("No data to send %p, %d", buf, len);
		return -1;
	}

	int length = len;
	int passed_len = 0;
	int write_len = 0;

	while (length > 0) {
		passed_len = send(fd, (const void *)buf, length, MSG_NOSIGNAL);
		if (passed_len == -1) {
			if (errno == EINTR)
				continue;
			else if (errno == EAGAIN)
				continue;
			else if (errno == EWOULDBLOCK)
				continue;
			ERROR("send error [%d]", errno);
			break;
		} else if (passed_len == 0)
			break;
		length -= passed_len;
		buf += passed_len;
	}
	write_len = len - length;

	if (write_len != len) {
		WARNING("WARNING: buf_size [%d] != write_len[%d]", len, write_len);
		return -1;
	}
	VERBOSE("write_len [%d]", write_len);

	return write_len;
}

int socket_recv(int fd, void **buf, unsigned int len)
{
	if (!buf) {
		INFO("Buffer must not null");
		return -1;
	}

	unsigned int length = len;
	int read_len = 0;
	int final_len = 0;
	char *temp = *buf;

	while (length > 0) {
		read_len = read(fd, (void *)temp, length);
		if (read_len < 0) {
			if (errno == EINTR)
				continue;
			else if (errno == EAGAIN)
				continue;
			else if (errno == EWOULDBLOCK)
				continue;
			else if (errno == EPIPE) {
				ERROR("connection closed : read err %d", errno, read_len, length);
				free(*buf);
				*buf = NULL;
				return 0; /* connection closed */
			}
			ERROR("read err %d, read_len :%d, length : %d", errno, read_len, length);
			final_len = read_len;
			break;
		} else if (read_len == 0)
			break;

		length -= read_len;
		temp += read_len;
	}

	if (final_len == 0)
		final_len = (len-length);

	if (len != final_len) {
		WARNING("WARNING: buf_size [%d] != read_len[%d]\n", read_len, final_len);
		return -1;
	}

	((char*)*buf)[len]= '\0';

	return final_len;
}

int socket_send_data(int fd, char *buf, unsigned int len)
{
	int ret = 0;
	int send_len = 0;
	int remain_len = len;

	if (len > MAX_ARRAY_LEN)
		INFO("send long data : length(%d) ++++++++++++++++++++++++", len);

	while (len > send_len) {
		if (remain_len > MAX_ARRAY_LEN)
			ret = socket_send(fd, (buf+send_len), MAX_ARRAY_LEN);
		else
			ret = socket_send(fd, (buf+send_len), remain_len);

		if (ret < 0) {
			ERROR("socket_send error");
			break;
		}
		send_len += ret;
		remain_len -= ret;
	}

	if (ret < 0) {
		ERROR("socket_send error");
		return -1;
	}

	return send_len;
}

int write_command(int fd, const uint64_t cmd)
{
	// poll : Level Trigger
	uint64_t clear_cmd = 0;
	int ret = write(fd, &clear_cmd, sizeof(clear_cmd));
	if (ret < 0)
		ERROR("write fail (%d)", ret);

	return write(fd, &cmd, sizeof(cmd));
}

int read_command(int fd, uint64_t *cmd)
{
	uint64_t dummy;
	int len = TEMP_FAILURE_RETRY(read(fd, &dummy, sizeof(dummy)));
	if (len == sizeof(dummy)) {
		*cmd = dummy;
	}
	return len;
}

