
#ifndef __CO_CTX_H__
#define __CO_CTX_H__
#include <stdlib.h>
#include <stdint.h>
#if defined(__arm64e__)
#error "libco does not support the arm64e pointer-authentication ABI"
#endif
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
#elif defined(__x86_64__) || defined(__aarch64__)
	void *regs[ 14 ];
#else
#error "Unsupported libco context architecture"
#endif
#if defined(__aarch64__)
    // x19..x30, sp, reserved; followed by the ABI-preserved halves of v8..v15.
    uint64_t fpregs[8];
    uint32_t fpcr;
    uint32_t fpsr;
#endif
	size_t ss_size;
	char *ss_sp;

};

int coctx_init( coctx_t *ctx );
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 );
#ifdef __cplusplus
extern "C" void coctx_swap(coctx_t *from, coctx_t *to);
#else
void coctx_swap(coctx_t *from, coctx_t *to);
#endif
#endif
