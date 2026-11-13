#!/usr/bin/env bash
# NEO-OS-3 system image builder.
#
# Builds the UEFI bootloader, the kernel and every ring-3 program, then packs
# them into a 64 MiB FAT32 EFI System Partition (`esp.img`).
#
# Usage:
#   ./tools/mkesp.sh          interactive/debug build (expects prebuilt binaries)
#   ./tools/mkesp.sh --ci     build everything from source (used by CI)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CI=0
[ "${1:-}" = "--ci" ] && CI=1

# Toolchain: prefer the pinned CI toolchain when present, else whatever
# `nightly` resolves to locally.
if rustup toolchain list | grep -q "nightly-2026-09-24"; then
    TC="nightly-2026-09-24"
else
    TC="nightly"
fi
echo "== toolchain: $TC =="

TARGET_KB="x86_64-unknown-none"
TARGET_EFI="x86_64-unknown-uefi"
PROFILE="debug"
[ "$CI" = "1" ] && PROFILE="release"

if [ "$CI" = "1" ]; then
    echo "== build bootloader ($TARGET_EFI) =="
    cargo +"$TC" build -p neo-boot --target "$TARGET_EFI" --release

    echo "== build kernel ($TARGET_KB) =="
    cargo +"$TC" build -p neo-kernel --target "$TARGET_KB" --release
fi

# ---------------------------------------------------------------------------
# User programs: one ELF per entry in tools/programs.list
# ---------------------------------------------------------------------------
if [ "$CI" = "1" ]; then
    echo "== build user programs ($TARGET_KB) =="
    (
        cd user
        export RUSTFLAGS="-C link-arg=-T$REPO_ROOT/user/user.ld -C linker=ld -C relocation-model=static"
        while read -r name feature; do
            case "$name" in ''|'#'*) continue ;; esac
            echo "  building $name..."
            if [ -n "${feature:-}" ]; then
                cargo +"$TC" build --target "$TARGET_KB" --release --features "$feature" --bin neo-user
            else
                cargo +"$TC" build --target "$TARGET_KB" --release --bin neo-user
            fi
            cp "target/$TARGET_KB/release/neo-user" "$REPO_ROOT/${name}.elf"
        done < "$REPO_ROOT/tools/programs.list"
    )
fi

# Resolve artefact paths (debug for local builds, release for CI).
EFI_OUT="$REPO_ROOT/target/$TARGET_EFI/$PROFILE/bootloader.efi"
[ -f "$EFI_OUT" ] || EFI_OUT="$REPO_ROOT/target/$TARGET_EFI/$PROFILE/bootloader"
KERNEL_ELF="$REPO_ROOT/target/$TARGET_KB/$PROFILE/neo-kernel"

if [ ! -f "$KERNEL_ELF" ]; then
    echo "ERROR: kernel binary not found at $KERNEL_ELF" >&2
    echo "       run: cargo build -p neo-kernel --target $TARGET_KB" >&2
    exit 1
fi

echo "== extract kernel binary =="
objcopy -O binary --set-section-flags .bss=alloc,load,contents \
  -R .user.text -R .user.rodata -R .user.data -R .user.bss \
  "$KERNEL_ELF" "$REPO_ROOT/KERNEL.BIN"

echo "== make FAT32 ESP =="
ESP="$REPO_ROOT/esp.img"
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=64 status=none
mkfs.vfat -F 32 -n NEOOS3 "$ESP" >/dev/null
mmd -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/NEO ::/BIN
mcopy -i "$ESP" "$EFI_OUT" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$REPO_ROOT/KERNEL.BIN" ::/EFI/NEO/KERNEL.BIN

# Default user image (the bootloader stages it as \EFI\NEO\USER.ELF).
if [ -f "$REPO_ROOT/user.elf" ]; then
    mcopy -i "$ESP" "$REPO_ROOT/user.elf" ::/EFI/NEO/USER.ELF
fi

# Every program goes to /BIN/ so the shell can spawn it by name.
for elf in "$REPO_ROOT"/*.elf; do
    [ -e "$elf" ] || continue
    base="$(basename "$elf" .elf)"
    mcopy -i "$ESP" "$elf" "::/BIN/$base"
done

rm -f "$REPO_ROOT/KERNEL.BIN"
echo "OK: esp.img ($(du -h "$ESP" | cut -f1))"
