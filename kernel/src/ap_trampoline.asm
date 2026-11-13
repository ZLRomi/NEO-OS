; NEO-OS-3 AP Trampoline
; Assembled with: nasm -f bin -o ap_trampoline.bin ap_trampoline.asm
BITS 16
org 0x8000

entry_16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    lgdt [gdt_ptr]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x08:entry_32

BITS 32
entry_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load CR3 (patched by BSP at offset 0xF0)
    mov eax, [PATCH_CR3]
    mov cr3, eax

    ; Enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    jmp 0x18:entry_64

BITS 64
entry_64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Get APIC ID first (needed for per-AP stack)
    mov eax, [0xFEE00020]
    shr eax, 24
    mov edi, eax                ; edi = APIC ID (passed to ap_entry)

    ; Calculate per-AP stack: stack_base + (apic_id + 1) * stack_size
    ; PATCH_STACK_BASE (offset 0x69): base address of AP stacks array
    ; PATCH_STACK_SIZE (offset 0x75): size per AP stack (e.g. 8192)
    mov rax, 0xDEADBEEFCAFEBABE  ; [0x69] patched to stack base
    mov rcx, 0xDEADBEEFCAFEBABE  ; [0x75] patched to per-AP stack size
    mov edx, edi
    inc edx                     ; (apic_id + 1) so stack grows downward from top
    imul rdx, rcx               ; (apic_id + 1) * stack_size
    add rax, rdx                ; stack_base + offset = stack top for this AP
    mov rsp, rax

    ; Jump to ap_entry (patched at offset 0x84).
    mov rax, 0xDEADBEEFCAFEBABE  ; [0x84] patched to ap_entry address
    jmp rax

.halt:
    hlt
    jmp .halt

align 8
gdt_ptr:
    dw (gdt_end - gdt_start - 1)
    dd gdt_start
gdt_start:
    dq 0                                    ; null
    dq 0x00CF9A000000FFFF                  ; 32-bit code (0x08)
    dq 0x00CF92000000FFFF                  ; 32-bit data (0x10)
    dq 0x00AF9A000000FFFF                  ; 64-bit code (0x18)
    dq 0x00AF92000000FFFF                  ; 64-bit data (0x20)
gdt_end:

; Patch points (must be at fixed offsets)
times 0xF0 - ($ - $$) db 0
PATCH_CR3:   dd 0xDEADBEEF           ; 0xF0: PML4 physical address
