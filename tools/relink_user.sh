#!/bin/bash
# Re-link neo-user ELF with custom linker script at 0x400000.
set -euo pipefail
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3

INPUT=target/x86_64-unknown-none/debug/neo-user
OUTPUT=target/x86_64-unknown-none/debug/neo-user-linked

# Extract sections into a relocatable object via objcopy + ld -r
objcopy -O elf64-x86-64 \
  -j .text -j .rodata -j .data -j .bss \
  --set-section-flags .bss=alloc,load,contents \
  "$INPUT" /tmp/user_sections.o

ld -m elf_x86_64 -r -o /tmp/user_rel.o /tmp/user_sections.o

# Final link with custom script
ld -m elf_x86_64 -T user/user.ld -o "$OUTPUT" /tmp/user_rel.o --no-dynamic-linker

echo "=== Linked ELF ==="
readelf -h "$OUTPUT" | grep -E 'Entry|Type'
readelf -l "$OUTPUT" | head -15

rm -f /tmp/user_sections.o /tmp/user_rel.o
