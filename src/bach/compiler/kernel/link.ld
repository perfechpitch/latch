/* Bach core 内 RV core 的链接脚本。
 *
 * ITCM 4 KB、DTCM 4 KB，容量照《RV Core MAS》F36。firmware 占开头，
 * 各 task 的入口跟在后面，它们的地址由链接器定，编完由 nm 导出给编译器
 * 填进 task_chain 的 TASK_PC。
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
  ITCM (rx)  : ORIGIN = 0x00000000, LENGTH = 4K
  DTCM (rw)  : ORIGIN = 0x00020000, LENGTH = 4K
}

/* 显式给出段头，否则 linux-gnu 工具链会生成没被 LOAD 覆盖的 PHDR */
PHDRS {
  text PT_LOAD FLAGS(5);   /* r-x */
  data PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS {
  .text : {
    *(.text.entry)          /* _start 必须在 reset pc 上 */
    *(.text.task.*)         /* 各 task 的入口，按名字排 */
    *(.text)
    *(.text.*)
  } > ITCM :text

  .rodata : { *(.rodata) *(.rodata.*) } > ITCM :text

  .data : {
    global_pointer = . + 0x800;
    *(.data) *(.data.*)
  } > DTCM :data

  .bss : {
    bss_start = .;
    *(.bss) *(.bss.*) *(COMMON)
    bss_end = .;
  } > DTCM :data

  .stack : {
    . = ALIGN(16);
    . += 1K;
    stack_top = .;
  } > DTCM :data

  /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
