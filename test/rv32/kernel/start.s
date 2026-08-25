# 定义入口点 _start，对应链接脚本的 ENTRY(_start)
.section .text.entry
.global _start

_start:
  # 初始化栈（使用链接脚本中定义的 stack_top）
  la sp, stack_top

  # 调用 main 函数
  call main

  # main 返回后进入死循环
loop:
  j loop
