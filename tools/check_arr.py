#!/usr/bin/env python3
import re

with open('/mnt/f/NEO-OS/kernel/font_misans.h', 'r', encoding='utf-8') as f:
    content = f.read()

codes_start = content.find('font_cn_codes[FONT_CN_COUNT] = {')
codes_end = content.find('};', codes_start)
codes_arr = content[codes_start:codes_end]
codes = re.findall(r'0x[0-9A-Fa-f]{4}', codes_arr)
print(f'font_cn_codes count: {len(codes)}')

cn_start = content.find('font_cn[FONT_CN_COUNT][FONT_HEIGHT * 2] = {')
cn_end = content.find('};', cn_start)
cn_arr = content[cn_start:cn_end]
cn_entries = re.findall(r'/\* U\+[0-9A-F]{4}', cn_arr)
print(f'font_cn entries: {len(cn_entries)}')
print(f'first 5: {cn_entries[:5]}')
print(f'last 5: {cn_entries[-5:]}')

if len(codes) != len(cn_entries):
    print(f'MISMATCH: codes={len(codes)} cn={len(cn_entries)}')
