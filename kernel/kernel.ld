/* NEO-OS v3 kernel link script: flat image linked at 1MiB. M8: sections
 * are 4K-aligned so per-page protection (.text U+X, .rodata U+RO, the
 * rest supervisor-only) never bleeds across a shared page. */
OUTPUT_FORMAT(elf64-x86-64);
ENTRY(_start);
SECTIONS {
  . = 0x400000;
  _text_start = .;
  .text : {
    *(.text._start)
    *(.text*)
  }
  _text_end = .;
  . = ALIGN(0x1000);
  _rodata_start = .;
  .rodata : { *(.rodata*) }
  _rodata_end = .;
  . = ALIGN(0x1000);
  .data : { *(.data*) *(.data.tmp_pagetable) }
  . = ALIGN(0x1000);
  .bss : {
    *(.bss*)
    *(COMMON)
  }
  _kernel_end = .;
  /* M10: ring3 programs at USER_LOAD_ADDR (0x800000). Must match
   * neo_common::USER_LOAD_ADDR. */
  .user.text 0x800000 : {
    _user_text_start = .;
    *(.user.text*)
    *(.user)           /* legacy: exact ".user" section (code + ro strings) */
    _user_text_end = .;
  }
  . = ALIGN(0x1000);
  .user.rodata : {
    _user_rodata_start = .;
    *(.user.rodata*)
    _user_rodata_end = .;
  }
  . = ALIGN(0x1000);
  .user.data : {
    _user_data_start = .;
    *(.user.data*)
    _user_data_end = .;
  }
  . = ALIGN(0x1000);
  .user.bss : {
    _user_bss_start = .;
    *(.user.bss*)
    _user_bss_end = .;
  }
  . = ALIGN(0x1000);
  _user_end = .;
  /DISCARD/ : {
    *(.eh_frame*)
    *(.comment*)
  }
}
