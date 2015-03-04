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


#ifndef __PIMS_DEBUG_H__
#define __PIMS_DEBUG_H__

#include <assert.h>

#define LOG_TAG     "PIMS_IPC"
#include <dlog.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PIMS_VERBOSE_TAG(frmt, args...) SLOGV(frmt, ##args);
#define PIMS_DEBUG_TAG(frmt, args...)   SLOGD(frmt, ##args);
#define PIMS_INFO_TAG(frmt, args...)    SLOGI(frmt, ##args);
#define PIMS_WARN_TAG(frmt, args...)    SLOGV(frmt, ##args);
#define PIMS_ERROR_TAG(frmt, args...)   SLOGE(frmt, ##args);


#define ENTER()	PIMS_DEBUG_TAG(">>>>>>>> called")
#define LEAVE()	PIMS_DEBUG_TAG("<<<<<<<< ended")

//#define VERBOSE(frmt, args...)  PIMS_VERBOSE_TAG(frmt, ##args)
#define VERBOSE(frmt, args...)
#define DEBUG(frmt, args...)    PIMS_DEBUG_TAG(frmt, ##args)
#define INFO(frmt, args...)     PIMS_INFO_TAG(frmt, ##args)
#define WARNING(frmt, args...)  PIMS_WARN_TAG(frmt, ##args)
#define ERROR(frmt, args...)    PIMS_ERROR_TAG(frmt, ##args)

#define WARN_IF(expr, fmt, arg...) do { \
	if (expr) { \
		ERROR(fmt, ##arg); \
	} \
} while (0)


#define ASSERT(expr) \
	if (!(expr)) \
	{   \
		ERROR("Assertion %s", #expr); \
	} \
	assert(expr)

#ifdef __cplusplus
}
#endif

#endif /* __PIMS_DEBUG_H__ */
