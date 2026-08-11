/* rv32 demo kernel 的链接脚本
 *
 * 地址空间与 src/module/rv32_pipeline.h 里的 Rv32Config 保持一致：
 *   ILM  0x80000000 .. 0x8003FFFF  256KB  取指存储器，只放代码和只读数据
 *   DLM  0x80040000 .. 0x8007FFFF  256KB  数据存储器，放 .data / .bss / 栈
 *   MMIO 0x10000000 ..             UART 与仿真控制寄存器
 *
 * .data 的加载地址（LMA）落在 ILM 里，运行地址（VMA）落在 DLM 里，
 * 由 start.S 在进 main 之前搬运一次。这样 objcopy -O binary 出来的
 * kernel.bin 就是一整块 ILM 镜像，直接按 0x80000000 装载即可。
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
  ILM (rx)  : ORIGIN = 0x80000000, LENGTH = 256K
  DLM (rwx) : ORIGIN = 0x80040000, LENGTH = 256K
}

STACK_SIZE = 16K;

SECTIONS {
  .text : {
    *(.text.entry)
    *(.text .text.*)
    . = ALIGN(4);
  } > ILM

  .rodata : {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(4);
  } > ILM

  .init_array : {
    init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    init_array_end = .;
    . = ALIGN(4);
  } > ILM

  data_lma = LOADADDR(.data);

  .data : {
    data_start = .;
    *(.data .data.*)
    *(.got.plt)
    *(.got)
    . = ALIGN(4);
    /* gp 指向 small data 区中部，一条 gp 相对指令即可覆盖 ±2KB */
    PROVIDE(__global_pointer$ = . + 0x800);
    *(.sdata .sdata.*)
    . = ALIGN(4);
    data_end = .;
  } > DLM AT> ILM

  .bss (NOLOAD) : {
    bss_start = .;
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    bss_end = .;
  } > DLM

  .stack (NOLOAD) : {
    . = ALIGN(16);
    stack_bottom = .;
    . += STACK_SIZE;
    stack_top = .;
  } > DLM

  ilm_end = ORIGIN(ILM) + LENGTH(ILM);
  dlm_end = ORIGIN(DLM) + LENGTH(DLM);

  ASSERT(data_lma + (data_end - data_start) <= ilm_end, "ILM overflow")
  ASSERT(stack_top <= dlm_end, "DLM overflow")

  /DISCARD/ : {
    *(.comment)
    *(.note .note.*)
    *(.eh_frame .eh_frame.*)
    *(.riscv.attributes)
  }
}
