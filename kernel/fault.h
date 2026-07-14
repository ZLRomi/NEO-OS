#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

void page_fault_isr(void* frame, uint32_t error_code);
void general_protection_isr(void* frame, uint32_t error_code);
void invalid_opcode_isr(void* frame);
void handle_user_page_fault(uint32_t addr, uint32_t error_code);

#endif
