/* rv32 demo kernel 的启动代码
 *
 * 复位后 CPU 从 ILM 起始地址 0x80000000 取第一条指令，也就是 _start。
 * 这里做四件事：立栈、把 .data 从 ILM 搬到 DLM、清 .bss、跑全局构造函数，
 * 然后进 main。main 返回后把返回值写进仿真退出寄存器，流水线随即停机。
 */

  .equ SIM_EXIT, 0x10001000

  .section .text.entry
  .globl _start
  .type _start, @function

_start:
  /* gp 用于 small data 的相对寻址，取 gp 时必须关掉 relaxation */
  .option push
  .option norelax
  la    gp, __global_pointer$
  .option pop

  la    sp, stack_top

  /* .data: ILM 上的加载副本 -> DLM 上的运行地址 */
  la    t0, data_lma
  la    t1, data_start
  la    t2, data_end
copy_data:
  bgeu  t1, t2, clear_bss
  lw    t3, 0(t0)
  sw    t3, 0(t1)
  addi  t0, t0, 4
  addi  t1, t1, 4
  j     copy_data

  /* .bss 清零 */
clear_bss:
  la    t0, bss_start
  la    t1, bss_end
clear_bss_loop:
  bgeu  t0, t1, run_init_array
  sw    zero, 0(t0)
  addi  t0, t0, 4
  j     clear_bss_loop

  /* 依次调用 .init_array 里的全局构造函数 */
run_init_array:
  la    s0, init_array_start
  la    s1, init_array_end
init_array_loop:
  bgeu  s0, s1, call_main
  lw    t0, 0(s0)
  jalr  ra, t0, 0
  addi  s0, s0, 4
  j     init_array_loop

call_main:
  li    a0, 0
  li    a1, 0
  call  main

  /* main 的返回值就是仿真退出码 */
  li    t0, SIM_EXIT
  sw    a0, 0(t0)

halt:
  j     halt

  .size _start, . - _start
