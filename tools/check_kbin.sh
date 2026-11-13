#!/bin/bash
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3
KE=target/x86_64-unknown-none/debug/neo-kernel
objcopy -O binary --set-section-flags .bss=alloc,load,contents \
  -R .user.text -R .user.rodata -R .user.data -R .user.bss "$KE" /tmp/k.bin
echo "=== KERNEL.BIN first 16 bytes ==="
xxd -l 16 /tmp/k.bin
echo "=== ELF at offset 0x1000 (file offset for VA 0x100000) ==="
xxd -s 0x1000 -l 16 "$KE"
echo "=== Sizes ==="
wc -c /tmp/k.bin
rm -f /tmp/k.bin
