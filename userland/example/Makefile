# NEO 应用编译工具链示例
# 用法: make            编译 hello.neap
#       make run        编译并部署到 ISO（需先在项目根 make iso）

CC      = gcc
CFLAGS  = -m32 -ffreestanding -O2 -Wall -Wextra \
          -fno-leading-underscore -fno-exceptions -mgeneral-regs-only \
          -fno-builtin -fno-tree-loop-distribute-patterns \
          -nostdlib -nostartfiles \
          -I. -I../libc -I../../kernel
LD      = ld
LDFLAGS = -m elf_i386 -T neap_app.ld -e _start

LIBC    = ../libc/libneolibc.a
APP     = hello.neap

all: $(APP)

$(APP): hello.c $(LIBC) neap_app.ld
	$(CC) $(CFLAGS) -c hello.c -o hello.o
	$(LD) $(LDFLAGS) -o hello.elf hello.o $(LIBC)
	python3 ../../tools/elf2neap.py hello.elf $(APP)

$(LIBC):
	$(MAKE) -C ../libc

run: $(APP)
	cp $(APP) ../../iso/apps/ 2>/dev/null || true
	cd ../.. && make iso

clean:
	rm -f hello.o hello.elf $(APP)

.PHONY: all run clean
