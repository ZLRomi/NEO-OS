#include "rtc.h"
#include "io.h"
#include "klog.h"
#include <stdint.h>

static int bcd_to_dec(int val) {
    return (val >> 4) * 10 + (val & 0x0F);
}

static int dec_to_bcd(int val) {
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg | 0x80);
    return inb(0x71);
}

static void cmos_write(uint8_t reg, uint8_t val) {
    outb(0x70, reg | 0x80);
    outb(0x71, val);
}

static void rtc_wait_uip(void) {
    while (cmos_read(0x0A) & 0x80) { }
}

void rtc_init(void) {
    rtc_time_t t;
    if (rtc_get_time(&t) == 0) {
        klog_write_level(KLOG_INFO, "rtc: %d-%d-%d %d:%d:%d\n",
                   t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        klog_write_str("rtc: init read failed\n");
    }
}

int rtc_get_time(rtc_time_t* t) {
    if (!t) return -1;

    rtc_wait_uip();
    int second  = cmos_read(0x00);
    int minute  = cmos_read(0x02);
    int hour    = cmos_read(0x04);
    int day     = cmos_read(0x07);
    int month   = cmos_read(0x08);
    int year    = cmos_read(0x09);
    int century = cmos_read(0x32);

    uint8_t regB = cmos_read(0x0B);
    int bcd = !(regB & 0x04);

    if (bcd) {
        second  = bcd_to_dec(second);
        minute  = bcd_to_dec(minute);
        hour    = bcd_to_dec(hour & 0x7F);
        day     = bcd_to_dec(day);
        month   = bcd_to_dec(month);
        year    = bcd_to_dec(year);
        if (century) century = bcd_to_dec(century);
    } else {
        hour = hour & 0x3F;
    }

    if (!(regB & 0x02) && (hour & 0x80)) {
        hour = (hour & 0x7F);
        if (hour) hour += 12;
    }

    int full_year;
    if (century) {
        full_year = century * 100 + year;
    } else {
        full_year = (year < 70) ? (2000 + year) : (1900 + year);
    }

    t->second = second;
    t->minute = minute;
    t->hour   = hour;
    t->day    = day;
    t->month  = month;
    t->year   = full_year;
    return 0;
}

int rtc_set_time(const rtc_time_t* t) {
    if (!t) return -1;

    uint8_t regB = cmos_read(0x0B);
    int bcd = !(regB & 0x04);

    int second, minute, hour, day, month, year;
    if (bcd) {
        second = dec_to_bcd(t->second);
        minute = dec_to_bcd(t->minute);
        hour   = dec_to_bcd(t->hour);
        day    = dec_to_bcd(t->day);
        month  = dec_to_bcd(t->month);
        year   = dec_to_bcd(t->year % 100);
    } else {
        second = t->second;
        minute = t->minute;
        hour   = t->hour;
        day    = t->day;
        month  = t->month;
        year   = t->year % 100;
    }

    cmos_write(0x00, (uint8_t)second);
    cmos_write(0x02, (uint8_t)minute);
    cmos_write(0x04, (uint8_t)hour);
    cmos_write(0x07, (uint8_t)day);
    cmos_write(0x08, (uint8_t)month);
    cmos_write(0x09, (uint8_t)year);

    int century = t->year / 100;
    if (century > 0) cmos_write(0x32, (uint8_t)(bcd ? dec_to_bcd(century) : century));

    (void)regB;
    return 0;
}
