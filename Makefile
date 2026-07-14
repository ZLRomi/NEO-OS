CC = gcc
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-leading-underscore -nostdlib -nostartfiles -mgeneral-regs-only -Ikernel
LD = ld
LDFLAGS = -m elf_i386 -T linker.ld
ASM = nasm
ASMFLAGS = -f elf32

CSRCS = $(wildcard kernel/*.c)
OBJS = build/boot.o $(CSRCS:kernel/%.c=build/%.o)

all: assets build/kernel.bin

assets:
	python3 tools/convert_assets.py

build/kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

build/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

build/boot.o: boot/boot.asm
	$(ASM) $(ASMFLAGS) $< -o $@

iso: all
	cp build/kernel.bin iso/boot/kernel.bin
	grub-mkrescue -o neo-os.iso iso/

run: iso
	qemu-system-x86_64 -cdrom neo-os.iso -vga vmware \
	-netdev user,id=net0 -device e1000,netdev=net0 \
	-object filter-dump,id=f1,netdev=net0,file=net.pcap \
	-audiodev none,id=snd -device ac97,audiodev=snd \
	-serial stdio

clean:
	rm -f build/*.o build/kernel.bin neo-os.iso

.PHONY: all clean run iso assets