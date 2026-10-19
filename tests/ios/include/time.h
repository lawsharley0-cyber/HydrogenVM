#pragma once
#include <stddef.h>
typedef long time_t;
typedef unsigned long clock_t;
struct timeval { long tv_sec; int tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; char *tm_zone; };
time_t time(time_t *);
clock_t clock(void);
int gettimeofday(struct timeval *, void *);
int nanosleep(const struct timespec *, struct timespec *);
struct tm *localtime(const time_t *);
struct tm *gmtime(const time_t *);
time_t mktime(struct tm *);
size_t strftime(char *, size_t, const char *, const struct tm *);
#define CLOCKS_PER_SEC 1000000
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 6
int clock_gettime(clockid_t, struct timespec *);
