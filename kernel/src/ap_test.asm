; Minimal AP test trampoline — just write a marker and halt.
; Assembled with: nasm -f bin -o ap_test.bin ap_test.asm
BITS 16
org 0x8000

; Write "AP!" to serial port (0x3F8) as a signal.
mov dx, 0x3F8
mov al, 'A'
out dx, al
mov al, 'P'
out dx, al
mov al, '!'
out dx, al
mov al, 13
out dx, al
mov al, 10
out dx, al

; Halt forever.
halt:
    hlt
    jmp halt
