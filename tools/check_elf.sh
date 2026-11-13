#!/bin/bash
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3
KE=target/x86_64-unknown-none/debug/neo-kernel
objcopy -O binary -j .user.text -j .user.rodata -j .user.data -j .user.bss \
  --set-section-flags .user.bss=alloc,load,contents "$KE" /tmp/uf.bin
objcopy -I binary -O elf64-x86-64 --change-addresses=0x400000 /tmp/uf.bin /tmp/ue.elf
printf '\x02\x00' | dd of=/tmp/ue.elf bs=1 seek=16 count=2 conv=notrunc status=none

echo "=== ELF header fields ==="
echo -n "e_shoff:    "; xxd -s 40 -l 8 -p /tmp/ue.elf
echo -n "e_shentsize:"; xxd -s 58 -l 2 -p /tmp/ue.elf
echo -n "e_shnum:    "; xxd -s 60 -l 2 -p /tmp/ue.elf

# Manually read section 1 (index 1, the .data section)
# e_shoff = 0x1118, e_shentsize should be 64 (0x40)
# Section 1 at offset 0x1118 + 64 = 0x1158
echo ""
echo "=== Section 1 raw bytes (64 bytes at 0x1158) ==="
xxd -s 0x1158 -l 64 /tmp/ue.elf

rm -f /tmp/uf.bin /tmp/ue.elf
