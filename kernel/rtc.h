#ifndef RTC_H
#define RTC_H

typedef struct {
    int year, month, day;
    int hour, minute, second;
} rtc_time_t;

void rtc_init(void);
int rtc_get_time(rtc_time_t* t);
int rtc_set_time(const rtc_time_t* t);

#endif
