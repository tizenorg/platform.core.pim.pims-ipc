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


#ifndef __PIMS_DEBUG_H__
#define __PIMS_DEBUG_H__

#ifndef _NON_SLP
#include <dlog.h>
#endif
#include <assert.h>

#ifdef _cplusplus
extern "C"
{
#endif

/* Tag defines */
#define TAG_IPC     "PIMS_IPC"

/* debug base macro */
#ifndef _NON_SLP
#define __ug_log(logtype, tag, frmt, args...) \
        do {LOG(logtype, tag, "%s:%s(%d) > "frmt"\n", __MODULE__, __FUNCTION__, __LINE__, ##args);} while (0)
#else
#define LOG_VERBOSE "VERBOSE"
#define LOG_DEBUG   "DEBUG"
#define LOG_INFO    "INFO"
#define LOG_WARN    "WARN"
#define LOG_ERROR   "ERROR"
#define __ug_log(logtype, tag, frmt, args...) \
        do {printf("[%s][%s][%08x] %s:%s(%d) > "frmt"\n", logtype, tag, (unsigned int)pthread_self(), __MODULE__, __FUNCTION__, __LINE__, ##args);} while (0)
#endif

#define pims_verbose(tag, frmt, args...) __ug_log(LOG_VERBOSE, tag, frmt, ##args)
#define pims_debug(tag, frmt, args...)   __ug_log(LOG_DEBUG,   tag, frmt, ##args)
#define pims_info(tag, frmt, args...)    __ug_log(LOG_INFO,    tag, frmt, ##args)
#define pims_warn(tag, frmt, args...)    __ug_log(LOG_WARN,    tag, frmt, ##args)
#define pims_error(tag, frmt, args...)   __ug_log(LOG_ERROR,   tag, frmt, ##args)

#ifndef TAG_NAME // SET default TAG
#define TAG_NAME    TAG_IPC
#endif

#define PIMS_VERBOSE_TAG(frmt, args...) pims_verbose(TAG_NAME, frmt, ##args);
#define PIMS_DEBUG_TAG(frmt, args...)   pims_debug  (TAG_NAME, frmt, ##args);
#define PIMS_INFO_TAG(frmt, args...)    pims_info   (TAG_NAME, frmt, ##args);
#define PIMS_WARN_TAG(frmt, args...)    pims_warn   (TAG_NAME, frmt, ##args);
#define PIMS_ERROR_TAG(frmt, args...)   pims_error  (TAG_NAME, frmt, ##args);

//#define VERBOSE(frmt, args...)  PIMS_VERBOSE_TAG(frmt, ##args)
#define VERBOSE(frmt, args...)
#define DEBUG(frmt, args...)    PIMS_DEBUG_TAG(frmt, ##args)
#define INFO(frmt, args...)     PIMS_INFO_TAG(frmt, ##args)
#define WARNING(frmt, args...)  PIMS_WARN_TAG(frmt, ##args)
#define ERROR(frmt, args...)    PIMS_ERROR_TAG(frmt, ##args)

#define ASSERT(expr) \
    if (!(expr)) \
    {   \
        ERROR("Assertion %s", #expr); \
    } \
    assert(expr)

#ifdef _cplusplus
}
#endif

#endif /* __PIMS_DEBUG_H__ */
