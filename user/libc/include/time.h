#ifndef USER_TIME_H
#define USER_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

#define CLOCKS_PER_SEC 1000000L

time_t time(time_t *t);
struct tm *gmtime(const time_t *tp);
struct tm *localtime(const time_t *tp);
char *ctime(const time_t *tp);
char *asctime(const struct tm *tm);
time_t mktime(struct tm *tm);
double difftime(time_t a, time_t b);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

#endif