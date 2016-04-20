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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <systemd/sd-daemon.h>
#include <sys/stat.h>

#include "pims-internal.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc-worker.h"
#include "pims-ipc-utils.h"
#include "pims-socket.h"

#define MAX_ARRAY_LEN 65535
#define PIMS_WAIT_MSEC 1000

static int _get_pid(int fd)
{
	int ret = 0;
	struct ucred uc = {0};
	socklen_t uc_len = sizeof(uc);
	ret = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &uc_len);
	if (ret < 0) {
		ERR("getsockopt() Fail(%d)", errno);
		return -1;
	}
	return uc.pid;
}

static bool _is_send_block(int fd)
{
	int ret = 0;
	int queue_size = 0;
	ioctl(fd, TIOCOUTQ, &queue_size);

	int buf_size = 0;
	int rn = sizeof(int);
	ret = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, (socklen_t *)&rn);
	if (ret < 0) {
		ERR("getsockopt() Fail(%d)", errno);
		DBG("remain size(%d)", queue_size);
		return false;
	}
	if (buf_size < queue_size) {
		DBG("send : buffer size(%d) < queue size(%d)", buf_size, queue_size);
		return true;
	}
	return false;
}

static int _sub_timespec_in_msec(struct timespec *st)
{
	struct timespec et = {0};
	clock_gettime(CLOCK_REALTIME, &et);

	/* 3 digits for sec, 3 digits for msec */
	int s_msec = ((st->tv_sec % 1000) * 1000) + (st->tv_nsec / 1000000);
	int e_msec = ((et.tv_sec % 1000) * 1000) + (et.tv_nsec / 1000000);
	return e_msec - s_msec;
}

int socket_send(int fd, char *buf, int len)
{
	int length = len;
	int passed_len = 0;
	int write_len = 0;

	bool retry = false;
	struct timespec st = {0};

	RETV_IF(NULL == buf, -1);
	RETVM_IF(len <= 0, -1, "Invalid length(%d)", len);

	while (length > 0) {
		passed_len = send(fd, (const void *)buf, length, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (passed_len == -1) {
			if (errno == EINTR) {
				ERR("EINTR error. send retry");
				continue;
			} else if (errno == EAGAIN) { /* same as EWOULDBLOCK */
				if (false == retry) {
					clock_gettime(CLOCK_REALTIME, &st);
					retry = true;
				} else {
					int diff_msec = _sub_timespec_in_msec(&st);
					if (PIMS_WAIT_MSEC < diff_msec) {
						ERR("EAGAIN error. send retry");
						DBG("send timestamp (%d.%d)sec and wait (%d)msec", st.tv_sec, st.tv_nsec, diff_msec);
						int pid = _get_pid(fd);
						if (true == _is_send_block(fd)) {
							DBG("send blocked. kill fd(%d) pid(%d)", fd, pid);
							return -1;
						} else {
							DBG("reset timeout and wait (%d)msec", PIMS_WAIT_MSEC);
							clock_gettime(CLOCK_REALTIME, &st);
						}
					}
				}
				continue;
			}
			ERR("send error [%d]", errno);
			break;
		} else if (passed_len == 0) {
			break;
		}

		length -= passed_len;
		buf += passed_len;

		retry = false;
	}
	write_len = len - length;

	if (write_len != len) {
		WARN("WARN: buf_size [%d] != write_len[%d]", len, write_len);
		return -1;
	}
	VERBOSE("write_len [%d]", write_len);

	return write_len;
}

int socket_recv(int fd, void **buf, unsigned int len)
{
	unsigned int length = len;
	int read_len = 0;
	int final_len = 0;
	char *temp = *buf;

	RETV_IF(NULL == *buf, -1);

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
				ERR("connection closed : read err %d", errno, read_len, length);
				free(*buf);
				*buf = NULL;
				return 0; /* connection closed */
			}
			ERR("read err %d, read_len :%d, length : %d", errno, read_len, length);
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
		WARN("WARN: buf_size [%d] != read_len[%d]\n", read_len, final_len);
		return -1;
	}

	((char*)*buf)[len] = '\0';

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
			ERR("socket_send error");
			break;
		}
		send_len += ret;
		remain_len -= ret;
	}

	if (ret < 0) {
		ERR("socket_send error");
		return -1;
	}

	return send_len;
}

int write_command(int fd, const uint64_t cmd)
{
	uint64_t clear_cmd = 0;
	int ret = write(fd, &clear_cmd, sizeof(clear_cmd));
	if (ret < 0)
		ERR("write fail (%d)", ret);

	return write(fd, &cmd, sizeof(cmd));
}

int read_command(int fd, uint64_t *cmd)
{
	uint64_t dummy;
	int len = TEMP_FAILURE_RETRY(read(fd, &dummy, sizeof(dummy)));
	if (len == sizeof(dummy))
		*cmd = dummy;

	return len;
}


/*
 * if delete = TRUE, steal client_id, then free(client_id)
 * if delete = FALSE, return client_id pointer, then do no call free(client_id
 */
static char* __find_client_id(pims_ipc_svc_s *ipc_svc, int client_fd, int delete)
{
	char *client_id;
	GList *cursor = NULL;
	pims_ipc_client_map_s *client;

	cursor = g_list_first(ipc_svc->client_id_fd_map);
	while (cursor) {
		client = cursor->data;
		if (client && client->fd == client_fd) {
			client_id = client->id;
			if (delete) {
				client->id = NULL;
				ipc_svc->client_id_fd_map = g_list_delete_link(ipc_svc->client_id_fd_map,
						cursor);
				free(client);
			}
			return client_id;
		}
		cursor = cursor->next;
	}
	return NULL;
}


static int __send_identify(int fd, unsigned int seq_no, char *id, int id_len)
{
	int total_len, length = 0;

	total_len = sizeof(total_len) + sizeof(id_len) + id_len + sizeof(seq_no);

	char buf[total_len+1];
	memset(buf, 0x0, total_len+1);

	memcpy(buf, &total_len, sizeof(total_len));
	length += sizeof(total_len);

	memcpy(buf+length, &id_len, sizeof(id_len));
	length += sizeof(id_len);
	memcpy(buf+length, id, id_len);
	length += id_len;

	memcpy(buf+length, &(seq_no), sizeof(seq_no));
	length += sizeof(seq_no);

	return socket_send(fd, buf, length);
}

static int __recv_raw_data(int fd, pims_ipc_raw_data_s **data, int *init)
{
	int len = 0;
	pims_ipc_raw_data_s *temp;

	/* read the size of message. note that ioctl is non-blocking */
	if (ioctl(fd, FIONREAD, &len)) {
		ERR("ioctl() Fail(%d)", errno);
		return -1;
	}

	/* when server or client closed socket */
	if (len == 0) {
		INFO("[IPC Socket] connection is closed");
		return 0;
	}

	temp = calloc(1, sizeof(pims_ipc_raw_data_s));
	if (NULL == temp) {
		ERR("calloc() Fail(%d)", errno);
		return -1;
	}
	temp->client_id = NULL;
	temp->client_id_len = 0;
	temp->call_id = NULL;
	temp->call_id_len = 0;
	temp->seq_no = 0;
	temp->has_data = FALSE;
	temp->data = NULL;
	temp->data_len = 0;

	int ret = 0;
	int read_len = 0;
	unsigned int total_len = 0;
	unsigned int has_data = FALSE;

	do {
		ret = TEMP_FAILURE_RETRY(read(fd, &total_len, sizeof(total_len)));
		if (-1 == ret) {
			ERR("read() Fail(%d)", errno);
			break;
		}
		read_len += ret;

		ret = TEMP_FAILURE_RETRY(read(fd, &(temp->client_id_len), sizeof(temp->client_id_len)));
		if (-1 == ret) {
			ERR("read() Fail(%d)", errno);
			break;
		}
		read_len += ret;

		temp->client_id = calloc(1, temp->client_id_len+1);
		if (NULL == temp->client_id) {
			ERR("calloc() Fail");
			return -1;
		}
		ret = socket_recv(fd, (void *)&(temp->client_id), temp->client_id_len);
		if (ret < 0) {
			ERR("socket_recv() Fail(%d)", ret);
			break;
		}
		read_len += ret;

		ret = TEMP_FAILURE_RETRY(read(fd, &(temp->seq_no), sizeof(temp->seq_no)));
		if (ret < 0) {
			ERR("read() Fail(%d)", ret);
			break;
		}
		read_len += ret;

		if (total_len == read_len) {
			*data = temp;
			*init = TRUE;
			return read_len;
		}

		ret  = TEMP_FAILURE_RETRY(read(fd, &(temp->call_id_len), sizeof(temp->call_id_len)));
		if (ret < 0) {
			ERR("read() Fail(%d)", errno);
			break;
		}
		read_len += ret;

		temp->call_id = calloc(1, temp->call_id_len+1);
		ret = socket_recv(fd, (void *)&(temp->call_id), temp->call_id_len);
		if (ret < 0) {
			ERR("socket_recv() Fail(%d)", ret);
			break;
		}
		read_len += ret;

		ret = TEMP_FAILURE_RETRY(read(fd, &has_data, sizeof(has_data)));
		if (ret < 0) {
			ERR("read() Fail(%d)", errno);
			break;
		}
		read_len += ret;

		if (has_data) {
			temp->has_data = TRUE;
			ret = TEMP_FAILURE_RETRY(read(fd, &(temp->data_len), sizeof(temp->data_len)));
			if (ret < 0) {
				ERR("read() Fail(%d)", errno);
				break;
			}
			read_len += ret;

			temp->data = calloc(1, temp->data_len+1);
			ret = socket_recv(fd, (void *)&(temp->data), temp->data_len);
			if (ret < 0) {
				ERR("socket_recv() Fail");
				break;
			}
			read_len += ret;
		}

		INFO("client_id : %s, call_id : %s, seq_no : %d", temp->client_id, temp->call_id,
				temp->seq_no);

		*data = temp;
		*init = FALSE;
	} while (0);

	if (ret < 0) {
		ERR("total_len(%d) client_id_len(%d)", total_len, temp->client_id_len);
		worker_free_raw_data(temp);
		*data = NULL;
		*init = FALSE;
		return -1;
	}

	return read_len;
}

static gboolean __process_init_request(int client_fd, pims_ipc_raw_data_s *req,
		pims_ipc_svc_s *ipc_svc)
{
	int ret;
	pims_ipc_client_map_s *client;

	client = calloc(1, sizeof(pims_ipc_client_map_s));
	if (NULL == client) {
		ERR("calloc() Fail(%d)", errno);
		return FALSE;
	}
	client->fd = client_fd;
	client->id = req->client_id;

	req->client_id = NULL;
	ipc_svc->client_id_fd_map = g_list_append(ipc_svc->client_id_fd_map, client);

	worker_start_idle_worker(ipc_svc);

	/* send server pid to client */
	char temp[100];
	snprintf(temp, sizeof(temp), "%d_%x", client_get_unique_sequence_number(), getpid());
	ret = __send_identify(client_fd, req->seq_no, temp, strlen(temp));

	worker_free_raw_data(req);
	if (-1 == ret) {
		ERR("__send_identify() Fail");
		return FALSE;
	}

	return TRUE;
}

static gboolean __process_request(int client_fd, pims_ipc_raw_data_s *req,
		pims_ipc_svc_s *ipc_svc)
{
	char *client_id = NULL;
	pims_ipc_worker_data_s *worker_data;

	client_id = __find_client_id(ipc_svc, client_fd, FALSE);
	if (NULL == client_id) {
		ERR("__find_client_id(%d) Fail", client_fd);
		return FALSE;
	}

	if (UTILS_STR_EQUAL == strcmp(PIMS_IPC_CALL_ID_CREATE, req->call_id)) {
		worker_data = worker_get_idle_worker(ipc_svc, client_id);
		if (NULL == worker_data) {
			ERR("worker_get_idle_worker() Fail");
			return FALSE;
		}
		if (!worker_data->fd) {
			int ret = worker_wait_idle_worker_ready(worker_data);
			if (ret < 0)
				return FALSE;
		}
	} else {
		worker_data = worker_find(ipc_svc, client_id);
	}

	if (worker_data) {
		worker_push_raw_data(worker_data, client_fd, req);
		write_command(worker_data->fd, 1);
	} else {
		ERR("worker_find(%s) Fail[client_fd(%d)]", client_id, client_fd);
	}
	return TRUE;
}

static gboolean __request_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	int client_fd;
	char *client_id = NULL;
	pims_ipc_svc_s *ipc_svc = data;

	RETV_IF(NULL == data, FALSE);

	client_fd = g_io_channel_unix_get_fd(src);

	if (G_IO_HUP & condition) {
		INFO("client closed: client_fd(%d)", client_fd);
		/* Find client_id */
		client_id = __find_client_id(ipc_svc, client_fd, TRUE);
		if (client_id) {
			worker_stop_client_worker(ipc_svc, client_id);
			free(client_id);
		}

		close(client_fd);
		return FALSE;
	}

	/* receive data from client */
	int recv_len;
	int init = FALSE;
	pims_ipc_raw_data_s *req = NULL;

	recv_len = __recv_raw_data(client_fd, &req, &init);
	if (0 < recv_len) {
		if (init)
			return __process_init_request(client_fd, req, ipc_svc);

		return __process_request(client_fd, req, ipc_svc);
	} else {
		ERR("receive invalid : %d", client_fd);
		close(client_fd);
		return FALSE;
	}
}


static gboolean __socket_handler(GIOChannel *src, GIOCondition condition, gpointer data)
{
	int sockfd;
	GIOChannel *channel;
	int client_sockfd = -1;
	struct sockaddr_un clientaddr;
	socklen_t client_len = sizeof(clientaddr);
	pims_ipc_svc_s *ipc_svc = data;

	sockfd = ipc_svc->sockfd;

	client_sockfd = accept(sockfd, (struct sockaddr *)&clientaddr, &client_len);
	if (-1 == client_sockfd) {
		char *errmsg = NULL;
		char buf[1024] = {0};
		errmsg = strerror_r(errno, buf, sizeof(buf));
		if (errmsg)
			ERROR("accept error : %s", errmsg);
		return TRUE;
	}

	channel = g_io_channel_unix_new(client_sockfd);
	g_io_add_watch(channel, G_IO_IN|G_IO_HUP, __request_handler, data);
	g_io_channel_unref(channel);

	return TRUE;
}

void socket_set_handler(void *user_data)
{
	int ret;
	struct sockaddr_un addr;
	GIOChannel *gio = NULL;
	pims_ipc_svc_s *ipc_svc = user_data;

	ret = sd_is_socket_unix(SD_LISTEN_FDS_START, SOCK_STREAM, -1, ipc_svc->service, 0);
	if (sd_listen_fds(1) == 1 && 0 < ret) {
		ipc_svc->sockfd = SD_LISTEN_FDS_START;
	} else {
		unlink(ipc_svc->service);
		ipc_svc->sockfd = socket(PF_UNIX, SOCK_STREAM, 0);

		bzero(&addr, sizeof(addr));
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_svc->service);

		ret = bind(ipc_svc->sockfd, (struct sockaddr *)&addr, sizeof(addr));
		if (ret != 0)
			ERR("bind() Fail(%d)", errno);
		ret = listen(ipc_svc->sockfd, 30);

		ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
		ret = chmod(ipc_svc->service, ipc_svc->mode);
	}

	gio = g_io_channel_unix_new(ipc_svc->sockfd);

	g_io_add_watch(gio, G_IO_IN, __socket_handler, ipc_svc);
}

