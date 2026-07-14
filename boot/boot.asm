BITS 32

section .multiboot
    align 4
    ; 魔数
    dd 0x1BADB002
    ; 标志：bit 1 = 内存信息
    dd 0x00000002
    ; 校验和
    dd -(0x1BADB002 + 0x00000002)

section .text
    global _start
    extern kernel_main

_start:
    cli
    mov esp, stack_top
    ; 将 multiboot_info 指针（EBX）保存到全局变量，供 C 使用
    mov [mb_info_ptr], ebx

    ; 加载自己的 GDT，避免依赖 GRUB 提供的段选择子
    lgdt [gdt_ptr]
    jmp 0x08:reload_segments
reload_segments:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    call kernel_main
    cli
    hlt
    jmp $

section .data
    global mb_info_ptr
mb_info_ptr dd 0

; 平面 4GB 代码段和数据段
align 8
gdt:
    dq 0x0000000000000000
gdt_code:
    dq 0x00CF9A000000FFFF
gdt_data:
    dq 0x00CF92000000FFFF
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

section .bss
    align 16
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
