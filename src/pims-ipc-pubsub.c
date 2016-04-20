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
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <glib.h>

#include "pims-internal.h"
#include "pims-ipc-data-internal.h"
#include "pims-ipc-utils.h"
#include "pims-socket.h"
#include "pims-ipc-pubsub.h"

typedef struct {
	char *service;
	gid_t group;
	mode_t mode;

	int publish_sockfd;
	int epoll_stop_thread;
	pthread_mutex_t subscribe_fds_mutex;
	GList *subscribe_fds;		/* client fd list */
} pims_ipc_svc_for_publish_s;

static pims_ipc_svc_for_publish_s *_g_singleton_for_publish = NULL;

static void __stop_for_publish(pims_ipc_svc_for_publish_s *ipc_svc)
{
	ipc_svc->epoll_stop_thread = TRUE;
}

static void* __publish_loop(void *user_data)
{
	int ret;
	int epfd;
	struct sockaddr_un addr;
	struct epoll_event ev = {0};
	pims_ipc_svc_for_publish_s *ipc_svc = user_data;

	unlink(ipc_svc->service);
	ipc_svc->publish_sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (ipc_svc->publish_sockfd < 0) {
		ERR("socket() Fail");
		return NULL;
	}

	bzero(&addr, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_svc->service);

	int flags = fcntl(ipc_svc->publish_sockfd, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	ret = fcntl(ipc_svc->publish_sockfd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0)
		ERR("fcntl() Fail(%d:%d)", ret, errno);
	VERBOSE("publish socketfd fcntl : %d\n", ret);

	ret = bind(ipc_svc->publish_sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret != 0)
		ERR("bind() Fail(%d)", ret);
	ret = listen(ipc_svc->publish_sockfd, 30);
	WARN_IF(ret != 0, "listen() Fail(%d)", ret);

	ret = chown(ipc_svc->service, getuid(), ipc_svc->group);
	WARN_IF(ret != 0, "chown() Fail(%d)", ret);
	ret = chmod(ipc_svc->service, ipc_svc->mode);
	WARN_IF(ret != 0, "chmod() Fail(%d)", ret);

	epfd = epoll_create(MAX_EPOLL_EVENT);

	ev.events = EPOLLIN | EPOLLHUP;
	ev.data.fd = ipc_svc->publish_sockfd;

	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, ipc_svc->publish_sockfd, &ev);
	WARN_IF(ret != 0, "epoll_ctl() Fail(%d)", ret);

	while (!ipc_svc->epoll_stop_thread) {
		int i = 0;
		struct epoll_event events[MAX_EPOLL_EVENT] = {{0}, };
		int event_num = epoll_wait(epfd, events, MAX_EPOLL_EVENT, -1);

		if (ipc_svc->epoll_stop_thread)
			break;

		if (event_num == -1) {
			if (errno != EINTR) {
				ERR("errno:%d\n", errno);
				break;
			}
		}

		for (i = 0; i < event_num; i++) {
			int event_fd = events[i].data.fd;

			if (events[i].events & EPOLLHUP) {
				VERBOSE("client closed ----------------------------------:%d", event_fd);
				if (epoll_ctl(epfd, EPOLL_CTL_DEL, event_fd, events) == -1)
					ERR("epoll_ctl(EPOLL_CTL_DEL) Fail(%d)", errno);

				close(event_fd);

				/* Find client_id and delete */
				GList *cursor = NULL;

				pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
				cursor = g_list_first(ipc_svc->subscribe_fds);
				while (cursor) {
					if (event_fd == (int)cursor->data) {
						ipc_svc->subscribe_fds = g_list_delete_link(ipc_svc->subscribe_fds, cursor);
						break;
					}
					cursor = g_list_next(cursor);
				}
				pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);
				continue;
			} else if (event_fd == ipc_svc->publish_sockfd) {
				/* connect client */
				struct sockaddr_un remote;
				socklen_t remote_len = sizeof(remote);
				int client_fd = accept(ipc_svc->publish_sockfd, (struct sockaddr *)&remote, &remote_len);
				if (client_fd == -1) {
					ERR("accept() Fail(%d)", errno);
					continue;
				}
				VERBOSE("client subscriber connect: %d", client_fd);

				pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
				ipc_svc->subscribe_fds = g_list_append(ipc_svc->subscribe_fds, GINT_TO_POINTER(client_fd));
				pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);

				ev.events = EPOLLIN;
				ev.data.fd = client_fd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
					ERR("epoll_ctl(EPOLL_CTL_ADD) Fail(%d)", errno);
					continue;
				}
			}
		}
	}

	close(ipc_svc->publish_sockfd);
	close(epfd);

	return NULL;
}

void pubsub_start()
{
	if (_g_singleton_for_publish)
		utils_launch_thread(__publish_loop, _g_singleton_for_publish);
}


void pubsub_stop()
{
	if (_g_singleton_for_publish)
		__stop_for_publish(_g_singleton_for_publish);
}

API int pims_ipc_svc_init_for_publish(char *service, gid_t group, mode_t mode)
{
	RETVM_IF(_g_singleton_for_publish, -1, "Already exist");

	_g_singleton_for_publish = g_new0(pims_ipc_svc_for_publish_s, 1);
	_g_singleton_for_publish->service = g_strdup(service);
	_g_singleton_for_publish->group = group;
	_g_singleton_for_publish->mode = mode;
	pthread_mutex_init(&_g_singleton_for_publish->subscribe_fds_mutex, 0);
	pthread_mutex_lock(&_g_singleton_for_publish->subscribe_fds_mutex);
	_g_singleton_for_publish->subscribe_fds = NULL;
	pthread_mutex_unlock(&_g_singleton_for_publish->subscribe_fds_mutex);

	return 0;
}

API int pims_ipc_svc_deinit_for_publish(void)
{
	if (!_g_singleton_for_publish)
		return -1;

	pthread_mutex_destroy(&_g_singleton_for_publish->subscribe_fds_mutex);
	g_list_free(_g_singleton_for_publish->subscribe_fds);

	g_free(_g_singleton_for_publish->service);
	g_free(_g_singleton_for_publish);
	_g_singleton_for_publish = NULL;

	return 0;
}

API int pims_ipc_svc_publish(char *module, char *event, pims_ipc_data_h data)
{
	gboolean is_valid = FALSE;
	unsigned int call_id_len;
	unsigned int is_data = FALSE;
	pims_ipc_data_s *data_in = data;
	gchar *call_id = PIMS_IPC_MAKE_CALL_ID(module, event);
	pims_ipc_svc_for_publish_s *ipc_svc = _g_singleton_for_publish;

	if (call_id)
		call_id_len = strlen(call_id);
	else
		call_id_len = 0;

	do {
		unsigned int len, total_len;
		len = sizeof(total_len) + sizeof(call_id_len) + call_id_len + sizeof(is_data);
		total_len = len;

		if (data_in) {
			is_data = TRUE;
			len += sizeof(data_in->buf_size);
			total_len = len + data_in->buf_size;
		}

		int length = 0;
		char buf[len+1];
		memset(buf, 0x0, len+1);

		memcpy(buf, &total_len, sizeof(total_len));
		length += sizeof(total_len);

		memcpy(buf+length, &call_id_len, sizeof(call_id_len));
		length += sizeof(call_id_len);
		memcpy(buf+length, call_id, call_id_len);
		length += call_id_len;
		g_free(call_id);

		memcpy(buf+length, &is_data, sizeof(is_data));
		length += sizeof(is_data);

		if (is_data) {
			memcpy(buf+length, &(data_in->buf_size), sizeof(data_in->buf_size));
			length += sizeof(data_in->buf_size);
		}

		/* Publish to clients */
		pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
		GList *cursor = g_list_first(ipc_svc->subscribe_fds);
		int ret = 0;
		while (cursor) {
			int fd = GPOINTER_TO_INT(cursor->data);
			pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);
			ret = socket_send(fd, buf, length);
			if (ret < 0)
				ERR("socket_send() Fail(%d)", ret);

			if (is_data) {
				ret = socket_send_data(fd, data_in->buf, data_in->buf_size);
				if (ret < 0)
					ERR("socket_send_data() Fail(%d)", ret);

			}
			pthread_mutex_lock(&ipc_svc->subscribe_fds_mutex);
			cursor = cursor->next;
		}
		pthread_mutex_unlock(&ipc_svc->subscribe_fds_mutex);

		is_valid = TRUE;
	} while (0);

	if (is_valid == FALSE)
		return -1;

	return 0;
}

