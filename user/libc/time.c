#include "time.h"
#include "sys/time.h"
#include "syscall.h"

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static int calc_wday(int year, int mon, int day) {
    if (mon < 3) { mon += 12; year--; }
    int k = year % 100, j = year / 100;
    int h = (day + 13 * (mon + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;
}

static struct tm g_tm;
struct tm *gmtime(const time_t *tp) {
    if (!tp) return 0;
    long secs = *tp;
    long days = secs / 86400;
    secs %= 86400;
    if (secs < 0) { days--; secs += 86400; }
    g_tm.tm_hour = (int)(secs / 3600); secs %= 3600;
    g_tm.tm_min  = (int)(secs / 60);
    g_tm.tm_sec  = (int)(secs % 60);
    int y = 1970;
    for (;;) {
        int yd = 365 + is_leap(y);
        if (days < yd) break;
        days -= yd; y++;
    }
    g_tm.tm_year = y - 1900;
    g_tm.tm_yday = (int)days;
    int m = 0;
    for (;;) {
        int md = mdays[m] + (m == 1 && is_leap(y));
        if (days < md) break;
        days -= md; m++;
    }
    g_tm.tm_mon  = m;
    g_tm.tm_mday = (int)days + 1;
    g_tm.tm_wday = calc_wday(y, m + 1, (int)days + 1);
    g_tm.tm_isdst = 0;
    return &g_tm;
}

struct tm *localtime(const time_t *tp) { return gmtime(tp); }

static char g_ctime[32];
char *ctime(const time_t *tp) {
    static const char wd[] = "SunMonTueWedThuFriSat";
    static const char mo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    struct tm *t = gmtime(tp);
    if (!t) return 0;
    int i = 0;
    for (int k = 0; k < 3; k++) g_ctime[i++] = wd[t->tm_wday * 3 + k];
    g_ctime[i++] = ' ';
    for (int k = 0; k < 3; k++) g_ctime[i++] = mo[t->tm_mon * 3 + k];
    g_ctime[i++] = ' ';
    int d = t->tm_mday;
    g_ctime[i++] = '0' + d / 10; g_ctime[i++] = '0' + d % 10;
    g_ctime[i++] = ' ';
    int h = t->tm_hour, mi = t->tm_min, s = t->tm_sec;
    g_ctime[i++] = '0' + h / 10;  g_ctime[i++] = '0' + h % 10;  g_ctime[i++] = ':';
    g_ctime[i++] = '0' + mi / 10; g_ctime[i++] = '0' + mi % 10; g_ctime[i++] = ':';
    g_ctime[i++] = '0' + s / 10;  g_ctime[i++] = '0' + s % 10;  g_ctime[i++] = ' ';
    int y = t->tm_year + 1900;
    g_ctime[i++] = '0' + y / 1000;
    g_ctime[i++] = '0' + (y / 100) % 10;
    g_ctime[i++] = '0' + (y / 10) % 10;
    g_ctime[i++] = '0' + y % 10;
    g_ctime[i++] = '\n';
    g_ctime[i] = 0;
    return g_ctime;
}

char *asctime(const struct tm *tm) {
    if (!tm) return 0;
    static const char wd[] = "SunMonTueWedThuFriSat";
    static const char mo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int i = 0;
    for (int k = 0; k < 3; k++) g_ctime[i++] = wd[tm->tm_wday * 3 + k];
    g_ctime[i++] = ' ';
    for (int k = 0; k < 3; k++) g_ctime[i++] = mo[tm->tm_mon * 3 + k];
    g_ctime[i++] = ' ';
    int d = tm->tm_mday;
    g_ctime[i++] = '0' + d / 10; g_ctime[i++] = '0' + d % 10;
    g_ctime[i++] = ' ';
    int h = tm->tm_hour, mi = tm->tm_min, s = tm->tm_sec;
    g_ctime[i++] = '0' + h / 10;  g_ctime[i++] = '0' + h % 10;  g_ctime[i++] = ':';
    g_ctime[i++] = '0' + mi / 10; g_ctime[i++] = '0' + mi % 10; g_ctime[i++] = ':';
    g_ctime[i++] = '0' + s / 10;  g_ctime[i++] = '0' + s % 10;  g_ctime[i++] = ' ';
    int y = tm->tm_year + 1900;
    g_ctime[i++] = '0' + y / 1000;
    g_ctime[i++] = '0' + (y / 100) % 10;
    g_ctime[i++] = '0' + (y / 10) % 10;
    g_ctime[i++] = '0' + y % 10;
    g_ctime[i++] = '\n';
    g_ctime[i] = 0;
    return g_ctime;
}

time_t mktime(struct tm *tm) {
    if (!tm) return -1;
    int y = tm->tm_year + 1900;
    long days = 0;
    for (int i = 1970; i < y; i++) days += 365 + is_leap(i);
    for (int i = 0; i < tm->tm_mon; i++) {
        days += mdays[i];
        if (i == 1 && is_leap(y)) days++;
    }
    days += tm->tm_mday - 1;
    return days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}

time_t time(time_t *t) {
    struct rtc_time rtc;
    time_t v = 0;
    if (rtc_get_time(&rtc) == 0) {
        struct tm tm;
        tm.tm_year = rtc.year - 1900;
        tm.tm_mon  = rtc.month - 1;
        tm.tm_mday = rtc.day;
        tm.tm_hour = rtc.hour;
        tm.tm_min  = rtc.minute;
        tm.tm_sec  = rtc.second;
        tm.tm_wday = 0; tm.tm_yday = 0; tm.tm_isdst = 0;
        v = mktime(&tm);
    }
    if (t) *t = v;
    return v;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    tv->tv_sec = time(0);
    tv->tv_usec = 0;
    return 0;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || max == 0 || !format || !tm) return 0;
    s[0] = '\0';
    return 0; // 返回空字符串让 NetSurf 有 fallback
}