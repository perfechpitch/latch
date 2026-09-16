
#include "coctx.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP 0
#define EIP 1
#define EAX 2
#define ECX 3

#define RSP 0
#define RIP 1
#define RBX 2
#define RDI 3
#define RSI 4

#define RBP 5
#define R12 6
#define R13 7
#define R14 8
#define R15 9
#define RDX 10
#define RCX 11
#define R8 12
#define R9 13

enum {
  kEIP = 0,
  kEBP = 6,
  kESP = 7,
};

enum {
  kRDI = 7,
  kRSI = 8,
  kRETAddr = 9,
  kRSP = 13,
};

#if defined(__x86_64__)
// coctx_swap.S 按偏移 112/120 存取这两个槽位，与 regs[14]/regs[15] 对应。
enum {
  kMXCSR = 14,
  kFPCW = 15,
};

// MXCSR 与 x87 控制字的 ABI 默认值（SSE2 初始状态 / 扩展精度、就近舍入）。
// memset 出来的 0 是个非法状态：它把所有浮点异常都解除屏蔽，第一次不精确
// 结果就会 SIGFPE。所以每次 memset 之后都要把这两个槽位填回默认值。
static const uint32_t kDefaultMXCSR = 0x1F80;
static const uint32_t kDefaultFPCW = 0x037F;

static void coctx_init_fp(coctx_t* ctx) {
  ctx->regs[kMXCSR] = (void*)(uintptr_t)kDefaultMXCSR;
  ctx->regs[kFPCW] = (void*)(uintptr_t)kDefaultFPCW;
}
#endif

extern "C" {
extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap");
};
#if defined(__i386__)
int coctx_init(coctx_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  return 0;
}
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {

  char* sp = ctx->ss_sp + ctx->ss_size - sizeof(coctx_param_t);
  sp = (char*)((unsigned long)sp & -16L);

  coctx_param_t* param = (coctx_param_t*)sp;
  void** ret_addr = (void**)(sp - sizeof(void*) * 2);
  *ret_addr = (void*)pfn;
  param->s1 = s;
  param->s2 = s1;

  memset(ctx->regs, 0, sizeof(ctx->regs));

  ctx->regs[kESP] = (char*)(sp) - sizeof(void*) * 2;
  return 0;
}
#elif defined(__x86_64__)
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
  char* sp = ctx->ss_sp + ctx->ss_size - sizeof(void*);
  sp = (char*)((unsigned long)sp & -16LL);

  memset(ctx->regs, 0, sizeof(ctx->regs));
  coctx_init_fp(ctx);
  void** ret_addr = (void**)(sp);
  *ret_addr = (void*)pfn;

  ctx->regs[kRSP] = sp;

  ctx->regs[kRETAddr] = (char*)pfn;

  ctx->regs[kRDI] = (char*)s;
  ctx->regs[kRSI] = (char*)s1;
  return 0;
}

int coctx_init(coctx_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  // 主伪协程的 ctx 会被 co_swap 保存、并在 co_yield_env 时恢复，初值同样要合法。
  coctx_init_fp(ctx);
  return 0;
}

#endif
