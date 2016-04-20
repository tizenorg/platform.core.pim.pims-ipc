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
#include <pthread.h>

#include "pims-internal.h"

void utils_launch_thread(void *(*start_routine)(void *), void *data)
{
	int ret = 0;

	pthread_t worker;
	pthread_attr_t attr;

	/* set kernel thread */
	ret = pthread_attr_init(&attr);
	if (0 != ret) {
		ERR("pthread_attr_init() Fail(%d)", ret);
		return;
	}
	ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
	if (0 != ret) {
		ERR("pthread_attr_setscope() Fail(%d)", ret);
		return;
	}
	ret = pthread_create(&worker, &attr, start_routine, data);
	if (0 != ret) {
		ERR("pthread_create() Fail(%d)", ret);
		return;
	}

	pthread_detach(worker);
}

