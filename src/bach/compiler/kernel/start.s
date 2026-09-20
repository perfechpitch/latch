# 三份 kernel 共用的入口。对应链接脚本的 ENTRY(_start)。
# firmware 跑完执行 WFT 进 wait，之后 TS 每下发一笔任务就跳到那笔的 TASK_PC。

.section .text.entry
.global _start

_start:
  la sp, stack_top          # 栈在 DTCM 顶
  la gp, global_pointer     # 全局指针，访问 .data 用

  call kernel_init          # 各 kernel 自己的初始化

wait_loop:
  # WFT：通知 TS 可以下发新任务，然后进 wait。
  #
  # funct3 取 011：custom-0 里 000（dsar/dsari）、001（dsaw/dsawi）、010
  # （task_done）、110（loop）都归了自定义指令，011 这一档是空的，拿它放 WFT。
  # 操作数字段全 0；bit31 留给“可选通知 IPI 向 SCP 上报状态”，与 task_done 用
  # bit31 放 ts 标志同一个形状。
  #
  # 这一条原先写作 `.insn i 0x0B, 1, x0, x0, 0`，那正好是 dsaw 的编码，会被
  # 译成“往 DSA 偏移 0 写 0”——VU 的偏移 0 就是 macro trigger_inst，真机上
  # 会误触发一条宏指令。改到 011 之后不再与那 6 条相撞。
  #
  # 模型不实现 WFT（firmware 那一段模型不跑，TS 直接跳到 task_pc），落到
  # SystemRv32 的译码表里当非法指令；等真机上给出 WFT 的语义再补。
  .insn r 0x0B, 3, 0, x0, x0, x0
  j wait_loop
