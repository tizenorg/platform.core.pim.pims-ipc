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


#ifndef __PIMS_DEBUG_H__
#define __PIMS_DEBUG_H__

#include <assert.h>

#define LOG_TAG "PIMS_IPC"
#include <dlog.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VERBOSE(frmt, args...)
#define DBG(frmt, args...) SLOGD(frmt, ##args)
#define INFO(frmt, args...) SLOGI(frmt, ##args)
#define WARN(frmt, args...) SLOGV(frmt, ##args)
#define ERR(frmt, args...) SLOGE(frmt, ##args)

#define FN_CALL() DBG(">>>>>>>> called")
#define FN_END() DBG("<<<<<<<< ended")

#define RET_IF(expr) do { \
	if (expr) { \
		ERR("(%s)", #expr); \
		return; \
	} \
} while (0)
#define RETV_IF(expr, val) do { \
	if (expr) { \
		ERR("(%s)", #expr); \
		return (val); \
	} \
} while (0)
#define RETM_IF(expr, fmt, arg...) do { \
	if (expr) { \
		ERR(fmt, ##arg); \
		return; \
	} \
} while (0)
#define RETVM_IF(expr, val, fmt, arg...) do { \
	if (expr) { \
		ERR(fmt, ##arg); \
		return (val); \
	} \
} while (0)

#define WARN_IF(expr, fmt, arg...) do { \
	if (expr) { \
		ERR(fmt, ##arg); \
	} \
} while (0)


#define ASSERT(expr) \
	if (!(expr)) {   \
		ERR("Assertion %s", #expr); \
	} \
	assert(expr)

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_DEBUG_H__ */
