/* NEO-OS-3 user-space linker script.
 * Links extracted .user* sections into a standalone ET_EXEC ELF with
 * proper PT_LOAD program headers and per-segment permissions.
 * Virtual addresses match the kernel's .user* layout (0x400000+). */
OUTPUT_FORMAT(elf64-x86-64);
ENTRY(user_prog);  /* default entry; spawn_user overrides via fn() pointer */

SECTIONS {
  . = 0x400000;

  .text : {
    *(.user.text*)
    *(.user)          /* legacy exact ".user" section */
  } :rx

  . = ALIGN(0x1000);
  .rodata : {
    *(.user.rodata*)
  } :r

  . = ALIGN(0x1000);
  .data : {
    *(.user.data*)
  } :rw

  . = ALIGN(0x1000);
  .bss : {
    *(.user.bss*)
  } :rw

  /DISCARD/ : {
    *(.eh_frame*)
    *(.comment*)
    *(.note*)
    *(.symtab*)
    *(.strtab*)
    *(.shstrtab*)
  }
}
