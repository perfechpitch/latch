# 三份 kernel 共用的入口。对应链接脚本的 ENTRY(_start)。
# firmware 跑完执行不通知 TS 的 task_done（F14）进 wait，之后 TS 每下发一笔
# 任务就跳到那笔的 TASK_PC。编码是 custom-0 funct3=010、bit31=0，不是 WFT。

.section .text.entry
.global _start

_start:
  la sp, stack_top          # 栈在 DTCM 顶
  la gp, global_pointer     # 全局指针，访问 .data 用

  call kernel_init          # 各 kernel 自己的初始化

  .word (0x0000000B | (0b010 << 12))  # task_done(ts=0)
