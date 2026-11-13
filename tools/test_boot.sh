#!/bin/bash
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3
rm -f /tmp/neo3_VARS.fd
cp /usr/share/edk2-ovmf/x64/OVMF_VARS.4m.fd /tmp/neo3_VARS.fd
chmod 644 /tmp/neo3_VARS.fd
CODE=/usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd
VARS=/tmp/neo3_VARS.fd
timeout 30 qemu-system-x86_64 \
  -machine q35 -cpu qemu64 -m 512 \
  -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
  -drive "if=pflash,format=raw,file=$VARS" \
  -drive file=esp.img,format=raw \
  -serial stdio -display none -monitor none 2>/dev/null | grep -a 'BOOT\|Hello\|Exception\|INFO' | head -15
