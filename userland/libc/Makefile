# NEO libc Makefile
# 编译为 freestanding 静态库 libneolibc.a，供用户态 .neap 应用链接。

CC      = gcc
CFLAGS  = -m32 -ffreestanding -O2 -Wall -Wextra \
          -fno-leading-underscore -fno-exceptions -mgeneral-regs-only \
          -fno-builtin -fno-tree-loop-distribute-patterns \
          -nostdlib -nostartfiles \
          -I. -I../../kernel
AR      = ar
ARFLAGS = rcs

C_SRCS   = syscalls.c string.c stdlib.c stdio.c unistd.c
ASM_SRCS = syscall.S
OBJS     = $(C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

LIB      = libneolibc.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS)

%.o: %.c neolibc.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S neolibc.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
