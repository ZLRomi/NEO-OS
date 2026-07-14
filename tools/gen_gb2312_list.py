#!/usr/bin/env python3
chars = []
for high in range(0xB0, 0xF8):
    for low in range(0xA1, 0xFF):
        try:
            b = bytes([high, low])
            ch = b.decode('gb2312')
            if '\u4e00' <= ch <= '\u9fff' and ch not in chars:
                chars.append(ch)
        except:
            pass
with open(r'/mnt/f/NEO-OS/tools/gb2312_chars.txt', 'w', encoding='utf-8') as f:
    for i in range(0, len(chars), 50):
        f.write(''.join(chars[i:i+50]) + '\n')
print(f'generated {len(chars)} chars')
