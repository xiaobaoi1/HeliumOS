#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* 与 user/libc/include/syscall.h 里 struct rtc_time 一致。
 * year 是完整年份（内核负责补世纪），用户态直接用。 */
struct rtc_time {
    uint16_t year;      /* 如 2026 */
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  second;    /* 0-59 */
    uint8_t  weekday;   /* 0-6，0=周日 */
    uint8_t  reserved;
};

void rtc_init(void);
int  rtc_read(struct rtc_time *out);   /* OK 或负错误码 */

#endif