#!/bin/bash
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3
KE=target/x86_64-unknown-none/debug/neo-kernel
objcopy -O binary --set-section-flags .bss=alloc,load,contents \
  -R .user.text -R .user.rodata -R .user.data -R .user.bss "$KE" /tmp/k.bin
echo "=== KERNEL.BIN size ==="
wc -c /tmp/k.bin
echo "=== First 32 bytes ==="
xxd -l 32 /tmp/k.bin
echo "=== _start offset in ELF ==="
nm "$KE" | grep ' _start$'
echo "=== Sections in kernel ELF ==="
readelf -S "$KE" | grep -E '\.text|\.rodata|\.data|\.bss|\.user'
rm -f /tmp/k.bin
