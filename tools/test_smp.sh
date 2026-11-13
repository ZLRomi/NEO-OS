#!/bin/bash
# Test SMP on multi-core QEMU.
cd /mnt/c/Users/Administrator/Desktop/NEO-OS-3
rm -f /tmp/neo3_VARS.fd
cp /usr/share/edk2-ovmf/x64/OVMF_VARS.4m.fd /tmp/neo3_VARS.fd
chmod 644 /tmp/neo3_VARS.fd
CODE=/usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd
VARS=/tmp/neo3_VARS.fd
timeout 30 qemu-system-x86_64 \
  -machine q35 -cpu max -m 512 -smp 4 \
  -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
  -drive "if=pflash,format=raw,file=$VARS" \
  -drive file=esp.img,format=raw \
  -serial file:/tmp/smp_serial.log \
  -display none -monitor tcp:127.0.0.1:4556,server=on,wait=off 2>/dev/null &
QEMU_PID=$!
sleep 3
echo "info cpus" | nc 127.0.0.1 4556 2>/dev/null || echo "nc not available"
wait $QEMU_PID 2>/dev/null || true
echo "=== Serial output ==="
grep -a 'BOOT\|Hello\|smp\|AP\|Exception' /tmp/smp_serial.log | head -20
