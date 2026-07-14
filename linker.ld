OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* 5 = R(4) + X(1) ，只读+执行 */
    data PT_LOAD FLAGS(6);  /* 6 = R(4) + W(2) ，读写，不可执行 */
}

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    } : text

    .rodata : ALIGN(4K)
    {
        *(.rodata)
    } : text

    .data : ALIGN(4K)
    {
        *(.data)
    } : data

    .bss : ALIGN(4K)
    {
        *(.bss)
    } : data

    _end = .;
}
