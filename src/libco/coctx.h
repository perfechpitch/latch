
#ifndef __CO_CTX_H__
#define __CO_CTX_H__
#include <stdlib.h>
typedef void* (*coctx_pfn_t)( void* s, void* s2 );
struct coctx_param_t
{
	const void *s1;
	const void *s2;
};
struct coctx_t
{
#if defined(__i386__)
	void *regs[ 8 ];
#else
	// 0..13 是通用寄存器与返回地址/栈指针（偏移 0..104，由 coctx_swap.S 使用）；
	// 14/15 是 MXCSR 与 x87 控制字（偏移 112/120）。这两样不在 SysV 的
	// caller-saved 集合里，编译器假定跨函数调用保持，所以必须由切换自己保存，
	// 否则一个协程改了舍入模式会泄漏给同线程的其他协程。
	void *regs[ 16 ];
#endif
	size_t ss_size;
	char *ss_sp;

};

int coctx_init( coctx_t *ctx );
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 );
#endif
