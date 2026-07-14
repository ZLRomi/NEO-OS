#ifndef CMD_H
#define CMD_H

#include <stdint.h>

extern const char* cmd_arg;

void cmd_parse(const char* input);
int  cmd_getline(char* buf, int max);

#endif
