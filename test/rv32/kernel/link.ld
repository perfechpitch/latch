/* link.ld - 自定义链接脚本 */
OUTPUT_ARCH(riscv)          /* 目标架构：RISC-V */
ENTRY(_start)               /* 程序入口点：_start 符号 */

/* 内存布局定义（根据实际硬件调整） */
MEMORY {
  RAM (rwxa) : ORIGIN = 0x80000000, LENGTH = 128M  /* 起始地址 0x80000000，大小 128MB */
}

/* 段布局 */
SECTIONS {
  .text : {                  /* 代码段：存放指令 */
    *(.text.entry)           /* 入口代码（如 _start）优先放在最开始 */
    *(.text)                 /* 其他代码 */
  } > RAM                    /* 加载到 RAM 区域 */

  .rodata : {                /* 只读数据段：常量、字符串等 */
    *(.rodata)
  } > RAM

  .data : {                  /* 数据段：初始化的全局变量 */
    *(.data)
  } > RAM

  .bss : {                   /* BSS 段：未初始化的全局变量（自动清零） */
    *(.bss)
  } > RAM

  /* 栈定义：从 RAM 高地址向下增长，大小 16KB */
  .stack : {
    . = ALIGN(16);           /* 栈地址 16 字节对齐 */
    . += 16K;                /* 留空 16KB */
    stack_top = .;           /* 栈顶地址符号 */
    . += 16K;                /* 栈大小 16KB */
    stack_bottom = .;        /* 栈底地址符号（供初始化用） */
  } > RAM
}