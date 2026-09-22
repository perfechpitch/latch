# 三份 kernel 共用的入口。对应链接脚本的 ENTRY(_start)。
# firmware 跑完执行 WFT 进 wait，之后 TS 每下发一笔任务就跳到那笔的 TASK_PC。

.section .text.entry
.global _start

_start:
  la sp, stack_top          # 栈在 DTCM 顶
  la gp, global_pointer     # 全局指针，访问 .data 用

  call kernel_init          # 各 kernel 自己的初始化

  .word (0x0000000B | (0b010 << 12))
