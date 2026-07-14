#ifndef POWER_H
#define POWER_H

#include <stdint.h>

void power_init(void);
void power_off(void);
void power_reboot(void);
int power_sleep(void);
int power_hibernate(void);

#endif
