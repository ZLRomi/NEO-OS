/* NEO-OS-3 user-space linker script (GNU ld).
 * Produces ET_EXEC ELF64 with PT_LOAD segments at 0x400000. */
OUTPUT_FORMAT(elf64-x86-64);
ENTRY(_start);

PHDRS {
  text PT_LOAD FLAGS(5);   /* PF_R | PF_X = 0x5 */
  rodata PT_LOAD FLAGS(4); /* PF_R = 0x4 */
  data PT_LOAD FLAGS(6);   /* PF_R | PF_W = 0x6 */
}

SECTIONS {
  . = 0x800000;

  .text : ALIGN(0x1000) {
    *(.text._start)
    *(.text*)
  } :text

  .rodata : ALIGN(0x1000) {
    *(.rodata*)
  } :rodata

  .data : ALIGN(0x1000) {
    *(.data*)
  } :data

  .bss : ALIGN(0x1000) {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  } :data

  /DISCARD/ : {
    *(.eh_frame*)
    *(.comment*)
    *(.note*)
  }
}
