#include <rtc.h>
#include <io.h>
#include <errno.h>
#include <printf.h>
#include <string.h>

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

#define RTC_SECOND  0x00
#define RTC_MINUTE  0x02
#define RTC_HOUR    0x04
#define RTC_WEEKDAY 0x06
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32

#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

/* 等 UIP（Update In Progress）清零 */
static void wait_not_updating(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(cmos_read(RTC_STATUS_A) & 0x80)) return;
    }
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

static void read_snapshot(uint8_t out[8]) {
    wait_not_updating();
    out[0] = cmos_read(RTC_SECOND);
    out[1] = cmos_read(RTC_MINUTE);
    out[2] = cmos_read(RTC_HOUR);
    out[3] = cmos_read(RTC_DAY);
    out[4] = cmos_read(RTC_MONTH);
    out[5] = cmos_read(RTC_YEAR);
    out[6] = cmos_read(RTC_WEEKDAY);
    out[7] = cmos_read(RTC_CENTURY);
}

void rtc_init(void) {
    kprintf("[RTC] Initialized\n");
}

int rtc_read(struct rtc_time *out) {
    if (!out) return EINVAL;

    /* 读两遍，两次一致才采用——防止更新撕裂 */
    uint8_t a[8], b[8];
    int tries = 0;
    do {
        read_snapshot(a);
        read_snapshot(b);
        if (memcmp(a, b, 8) == 0) break;
        tries++;
    } while (tries < 8);
    if (tries == 8) return EIO;

    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int is_bcd = !(status_b & 0x04);
    int is_24h = (status_b & 0x02) != 0;

    /* 小时：先处理 12 小时制的 PM 位，再 BCD 转换 */
    uint8_t hour_raw = a[2];
    int pm = 0;
    if (!is_24h) {
        pm = (hour_raw & 0x80) != 0;
        hour_raw &= 0x7F;
    }

    uint8_t sec  = is_bcd ? bcd_to_bin(a[0]) : a[0];
    uint8_t min  = is_bcd ? bcd_to_bin(a[1]) : a[1];
    uint8_t hour = is_bcd ? bcd_to_bin(hour_raw) : hour_raw;
    uint8_t day  = is_bcd ? bcd_to_bin(a[3]) : a[3];
    uint8_t mon  = is_bcd ? bcd_to_bin(a[4]) : a[4];
    uint8_t yr   = is_bcd ? bcd_to_bin(a[5]) : a[5];
    uint8_t wd   = is_bcd ? bcd_to_bin(a[6]) : a[6];
    uint8_t cent = a[7];

    /* 部分机器没有 century 寄存器——读到 0 或 0xFF 视为无效 */
    if (cent == 0 || cent == 0xFF) cent = 20;
    else if (is_bcd) cent = bcd_to_bin(cent);

    if (!is_24h) {
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    out->second   = sec;
    out->minute   = min;
    out->hour     = hour;
    out->day      = day;
    out->month    = mon;
    out->weekday  = wd;
    out->year     = (uint16_t)(cent * 100 + yr);
    out->reserved = 0;
    return OK;
}