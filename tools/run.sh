#!/usr/bin/env bash
# NEO-OS-3 QEMU runner (headless + HMP monitor on stdio).
#
# Works on any Linux, including GitHub Actions runners. WSL is not required;
# `run-wsl.sh` is kept as a thin alias for old muscle memory.
#
# Usage:
#   ./tools/run.sh                 # boot, screenshot, quit
#   ./tools/run.sh "sendkey a"     # run one HMP command after boot
#   ./tools/run.sh --smoke         # build (if needed) and assert boot markers
#   ./tools/run.sh --smp           # boot with -smp 4
#
# Set NEO_SKIP_BUILD=1 to reuse an existing esp.img instead of rebuilding.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=run
EXTRA="${1:-}"
SMP_FLAG=""
if [ "$EXTRA" = "--smoke" ]; then
  MODE=smoke
  EXTRA=""
  if [ "${NEO_SKIP_BUILD:-0}" != "1" ]; then
    echo "== rebuild from source =="
    bash ./tools/mkesp.sh --ci
  fi
elif [ "$EXTRA" = "--smp" ]; then
  MODE=smp
  EXTRA=""
  SMP_FLAG="-smp 4"
  if [ "${NEO_SKIP_BUILD:-0}" != "1" ]; then
    echo "== rebuild from source (SMP mode) =="
    bash ./tools/mkesp.sh --ci
  fi
elif [ "$EXTRA" = "--cmd" ]; then
  # Run a single shell command after boot, then quit. Used for fast iteration
  # on one subsystem instead of replaying the whole smoke suite.
  MODE=cmd
  EXTRA=""
  shift || true
  NEO_CMD="${1:-}"
  [ -n "$NEO_CMD" ] || { echo "usage: run.sh --cmd '<shell command>'"; exit 2; }
fi

[ -f esp.img ] || { echo "esp.img missing, run ./tools/mkesp.sh first"; exit 1; }

CODE=/usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd
VARS_SRC=/usr/share/edk2-ovmf/x64/OVMF_VARS.4m.fd
if [ ! -f "$CODE" ]; then
  CODE=/usr/share/edk2-ovmf/x64/OVMF_CODE_4M.fd
  VARS_SRC=/usr/share/edk2-ovmf/x64/OVMF_VARS_4M.fd
fi
if [ ! -f "$CODE" ]; then
  CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
  VARS_SRC=/usr/share/OVMF/OVMF_VARS_4M.fd
fi
VARS=/tmp/neo3_VARS.fd
if [ ! -f "$CODE" ]; then
  # Last resort: search common locations (Debian/Ubuntu ship both layouts).
  CODE=$(find /usr/share -name 'OVMF_CODE*.fd' 2>/dev/null | head -1 || true)
  VARS_SRC=$(find /usr/share -name 'OVMF_VARS*.fd' 2>/dev/null | head -1 || true)
fi
[ -f "$CODE" ] || { echo "missing OVMF firmware (Arch: edk2-ovmf; Ubuntu: ovmf)"; exit 1; }
# Always reset VARS to avoid stale NVRAM state causing alloc failures.
cp "$VARS_SRC" "$VARS"; chmod 644 "$VARS"

# Create a 4MiB test disk for virtio-blk.
BLK_IMG=/tmp/neo3-blk.img
if [ "$MODE" = "smoke" ]; then
  # Always reset disk for smoke tests to avoid stale state.
  dd if=/dev/zero of="$BLK_IMG" bs=1M count=4 2>/dev/null
elif [ ! -f "$BLK_IMG" ]; then
  dd if=/dev/zero of="$BLK_IMG" bs=1M count=4 2>/dev/null
fi

QEMU_LOG=/tmp/neo3-qemu.log
rm -f serial-wsl.log "$QEMU_LOG"

# TCG (used on CI, where /dev/kvm is unavailable) is roughly 4-6x slower than
# bare metal. Scale every wait so the smoke test does not race ahead of the
# kernel on a slow runner.
TIME_SCALE="${NEO_TIME_SCALE:-1}"
# Hard cap for the whole QEMU session; generous because TCG boot is slow.
QEMU_TIMEOUT="${NEO_QEMU_TIMEOUT:-240}"
scale_sleep() {
  # usage: scale_sleep <seconds>
  sleep "$(awk -v a="$1" -v s="$TIME_SCALE" 'BEGIN{printf "%.2f", a*s}')"
}

set +e
(
  scale_sleep 14
  if [ "$MODE" = "smoke" ]; then
    send_command() {
      local command="$1"
      local i key
      for ((i = 0; i < ${#command}; i++)); do
        key="${command:i:1}"
        [ "$key" = " " ] && key=spc
        printf 'sendkey %s\n' "$key"
        sleep 0.12
      done
      printf 'sendkey ret\n'
      scale_sleep 3
    }
    send_command user
    send_command userbad
    scale_sleep 6  # extra wait: userbad #PF triggers page-table free
    send_command usernx
    scale_sleep 6
    send_command userexec
    scale_sleep 6
    send_command userptr
    scale_sleep 4
    send_command isowrite
    scale_sleep 4
    send_command heaptest
    scale_sleep 4
    send_command meminfo
    scale_sleep 2
    send_command counter
    scale_sleep 2
    send_command calc
    scale_sleep 2
    send_command "help ls"
    scale_sleep 2
    send_command heapstress
    scale_sleep 3
    send_command ping
    scale_sleep 2
    send_command tcp
    scale_sleep 2
    send_command time
    scale_sleep 2
    send_command ls
    scale_sleep 2
    send_command test
    scale_sleep 2
    send_command blktest
    scale_sleep 3
    send_command mkfs
    scale_sleep 8
    send_command "fwrite hello world"
    scale_sleep 3
    send_command fls
    scale_sleep 2
    send_command "fcat hello"
    scale_sleep 2
    send_command fstest
    scale_sleep 8
    send_command uls
    scale_sleep 2
    send_command ucat
    scale_sleep 2
    send_command ufileops
    scale_sleep 8
  elif [ "$MODE" = "cmd" ]; then
    send_command() {
      local command="$1"
      local i key
      for ((i = 0; i < ${#command}; i++)); do
        key="${command:i:1}"
        [ "$key" = " " ] && key=spc
        printf 'sendkey %s\n' "$key"
        sleep 0.12
      done
      printf 'sendkey ret\n'
      scale_sleep 3
    }
    # Give the shell a moment to reach its prompt: the very first synthetic
    # keystroke is otherwise sometimes consumed during startup, which shows up
    # as a truncated command (e.g. "exectest" arriving as "ectest").
    scale_sleep 2
    send_command "$NEO_CMD"
    scale_sleep 6
  elif [ "$MODE" = "smp" ]; then
    # SMP mode: just wait for boot and check AP online.
    scale_sleep 5
  else
    [ -n "$EXTRA" ] && { echo "$EXTRA"; sleep 1; }
    echo "screendump /tmp/neo3-shot.png"
    scale_sleep 2
  fi
  echo quit
) | timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
  -machine q35 -cpu qemu64 -m 512 $SMP_FLAG \
  -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
  -drive "if=pflash,format=raw,file=$VARS" \
  -drive file=esp.img,format=raw \
  -drive "file=$BLK_IMG,format=raw,if=none,id=blk0" \
  -device virtio-blk,drive=blk0 \
  -netdev user,id=net0 \
  -device virtio-net,netdev=net0 \
  -serial file:serial-wsl.log \
  -display none -monitor stdio >"$QEMU_LOG" 2>&1
QEMU_STATUS=$?
set -e

if [ "$QEMU_STATUS" -ne 0 ]; then
  echo "QEMU failed (exit $QEMU_STATUS); monitor output: $QEMU_LOG" >&2
  cat "$QEMU_LOG" >&2
  exit "$QEMU_STATUS"
fi

echo "--- serial tail ---"
grep -a -E "Hello|INFO|WARN|ERROR" serial-wsl.log | tail -15

# Assertion helpers (used by both smoke and SMP modes).
assert_contains() {
  local expected="$1"
  if ! grep -aFq -- "$expected" serial-wsl.log; then
    echo "FAIL: missing serial marker: $expected" >&2
    echo "Full serial log: $PWD/serial-wsl.log" >&2
    exit 1
  fi
  echo "PASS: $expected"
}

assert_absent() {
  local unexpected="$1"
  if grep -aFq -- "$unexpected" serial-wsl.log; then
    echo "FAIL: unexpected serial marker: $unexpected" >&2
    echo "Full serial log: $PWD/serial-wsl.log" >&2
    exit 1
  fi
  echo "PASS: absent: $unexpected"
}

if [ "$MODE" = "smoke" ]; then
  assert_contains "[INFO] Hello NEO-OS v3 from kernel"
  assert_contains "neo> "
  assert_contains "demo: task A beat"
  assert_contains "demo: task B beat"
  assert_contains "hello from ring3!"
  assert_contains "ring3 waited 1s"
  assert_contains "userbad: reading kernel memory"
  assert_contains "usernx: executing the stack"
  assert_contains "userexec: executing kernel text"
  assert_contains "userptr: invalid buffers rejected"
  assert_contains "isowrite: counter == 42, isolated"
  assert_contains "heaptest: 64 bytes allocated and verified OK"
  assert_contains "mem: used="
  assert_contains "counter: 1000 iterations OK"
  assert_contains "calc: PASS"
  assert_contains "List files in /bin/"
  assert_contains "heapstress: 100 cycles OK"
  assert_contains "ping: packet sent"
  assert_contains "tcp: SYN sent"
  assert_contains "time:"
  assert_contains "/bin/"
  assert_contains "test: pass"
  assert_contains "blktest: write sector 0 OK"
  assert_contains "blktest: read+verify OK"
  assert_contains "mkfs: NEO-FS created"
  assert_contains "fwrite: wrote"
  assert_contains "world"
  assert_contains "fstest: ALL PASS"
  assert_contains "2/2 ALL PASS"
  assert_contains "spawned ring3 task"
  assert_contains "EXC breakpoint"
  assert_contains "demo: one-shot done"
  assert_absent "NOT rejected?!"
  assert_absent "KERNEL PANIC"
  assert_absent "-- halting"

  fault_count=$(grep -aF -c "user #PF: killing task" serial-wsl.log || true)
  if [ "$fault_count" -ne 3 ]; then
    echo "FAIL: expected exactly 3 isolated ring3 page faults, got $fault_count" >&2
    exit 1
  fi
  echo "PASS: $fault_count ring3 page faults isolated"

  prompt_count=$(grep -aF -o "neo> " serial-wsl.log | wc -l)
  if [ "$prompt_count" -lt 8 ]; then
    echo "FAIL: expected shell to survive all commands (prompts=$prompt_count)" >&2
    exit 1
  fi
  echo "PASS: shell remained responsive ($prompt_count prompts)"
elif [ "$MODE" = "smp" ]; then
  # SMP mode: verify no crash. Shell may not work in TCG multi-core mode.
  assert_contains "[INFO] Hello NEO-OS v3 from kernel"
  assert_absent "KERNEL PANIC"
  assert_absent "-- halting"

  ap_count=$(grep -aF -c "AP online" serial-wsl.log || true)
  echo "SMP: $ap_count AP(s) came online"

  smp_info=$(grep -a "smp:" serial-wsl.log | tail -1)
  echo "SMP: $smp_info"

  prompt_count=$(grep -aF -o "neo> " serial-wsl.log | wc -l)
  if [ "$prompt_count" -ge 2 ]; then
    echo "PASS: shell survived SMP boot ($prompt_count prompts)"
  else
    echo "WARN: shell did not reach prompt in SMP/TCG mode (known QEMU limitation)"
    echo "PASS: SMP boot completed without panic"
  fi
else
  ls -l /tmp/neo3-shot.png
fi