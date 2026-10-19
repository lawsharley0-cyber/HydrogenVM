// Minimal libdispatch declarations for HydrogenVM's test apps (OS_OBJECT_USE_OBJC style).
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __OBJC__
#import <objc/NSObject.h>
@protocol OS_dispatch_object <NSObject> @end
@protocol OS_dispatch_queue <OS_dispatch_object> @end
@protocol OS_dispatch_group <OS_dispatch_object> @end
@protocol OS_dispatch_semaphore <OS_dispatch_object> @end
@protocol OS_dispatch_source <OS_dispatch_object> @end
typedef NSObject<OS_dispatch_queue> *dispatch_queue_t;
typedef NSObject<OS_dispatch_group> *dispatch_group_t;
typedef NSObject<OS_dispatch_semaphore> *dispatch_semaphore_t;
typedef NSObject<OS_dispatch_source> *dispatch_source_t;
typedef NSObject<OS_dispatch_object> *dispatch_object_t;
#else
typedef struct dispatch_queue_s *dispatch_queue_t;
typedef struct dispatch_group_s *dispatch_group_t;
typedef struct dispatch_semaphore_s *dispatch_semaphore_t;
typedef struct dispatch_source_s *dispatch_source_t;
typedef void *dispatch_object_t;
#endif
typedef void (^dispatch_block_t)(void);
typedef void (*dispatch_function_t)(void *);
typedef uint64_t dispatch_time_t;
typedef long dispatch_once_t;
typedef const struct dispatch_source_type_s *dispatch_source_type_t;
typedef struct dispatch_queue_attr_s *dispatch_queue_attr_t;
#define DISPATCH_TIME_NOW (0ull)
#define DISPATCH_TIME_FOREVER (~0ull)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define DISPATCH_QUEUE_PRIORITY_HIGH 2
#define DISPATCH_QUEUE_PRIORITY_DEFAULT 0
#define DISPATCH_QUEUE_PRIORITY_LOW (-2)
#define DISPATCH_QUEUE_PRIORITY_BACKGROUND (-32768)
#define DISPATCH_QUEUE_SERIAL NULL
extern struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
#define DISPATCH_QUEUE_CONCURRENT (&_dispatch_queue_attr_concurrent)
extern struct dispatch_queue_s _dispatch_main_q;
#define dispatch_get_main_queue() ((__bridge dispatch_queue_t)(void *)&_dispatch_main_q)
extern const struct dispatch_source_type_s _dispatch_source_type_timer;
#define DISPATCH_SOURCE_TYPE_TIMER (&_dispatch_source_type_timer)
extern const struct dispatch_source_type_s _dispatch_source_type_data_add;
#define DISPATCH_SOURCE_TYPE_DATA_ADD (&_dispatch_source_type_data_add)
dispatch_queue_t dispatch_get_global_queue(long priority, unsigned long flags);
dispatch_queue_t dispatch_queue_create(const char *label, dispatch_queue_attr_t attr);
dispatch_queue_t dispatch_get_current_queue(void);
const char *dispatch_queue_get_label(dispatch_queue_t q);
void dispatch_async(dispatch_queue_t q, dispatch_block_t block);
void dispatch_async_f(dispatch_queue_t q, void *ctx, dispatch_function_t fn);
void dispatch_sync(dispatch_queue_t q, dispatch_block_t block);
void dispatch_barrier_async(dispatch_queue_t q, dispatch_block_t block);
void dispatch_barrier_sync(dispatch_queue_t q, dispatch_block_t block);
void dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t block);
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
void _dispatch_once(dispatch_once_t *pred, dispatch_block_t block);
void dispatch_once(dispatch_once_t *pred, dispatch_block_t block);
void dispatch_apply(size_t n, dispatch_queue_t q, void (^block)(size_t));
dispatch_group_t dispatch_group_create(void);
void dispatch_group_async(dispatch_group_t g, dispatch_queue_t q, dispatch_block_t block);
long dispatch_group_wait(dispatch_group_t g, dispatch_time_t timeout);
void dispatch_group_notify(dispatch_group_t g, dispatch_queue_t q, dispatch_block_t block);
void dispatch_group_enter(dispatch_group_t g);
void dispatch_group_leave(dispatch_group_t g);
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
dispatch_source_t dispatch_source_create(dispatch_source_type_t type, uintptr_t handle, unsigned long mask, dispatch_queue_t q);
void dispatch_source_set_timer(dispatch_source_t s, dispatch_time_t start, uint64_t interval, uint64_t leeway);
void dispatch_source_set_event_handler(dispatch_source_t s, dispatch_block_t handler);
void dispatch_source_cancel(dispatch_source_t s);
unsigned long dispatch_source_get_data(dispatch_source_t s);
void dispatch_source_merge_data(dispatch_source_t s, unsigned long value);
void dispatch_resume(dispatch_object_t o);
void dispatch_suspend(dispatch_object_t o);
void dispatch_queue_set_specific(dispatch_queue_t q, const void *key, void *ctx, dispatch_function_t destructor);
void *dispatch_get_specific(const void *key);
