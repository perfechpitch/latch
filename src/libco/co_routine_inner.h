
#ifndef __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t
{
	void *value;
};

struct stStackMem_t
{
	stCoRoutine_t* occupy_co;
	int stack_size;
	char* stack_bp;
	char* stack_buffer;

};

struct stShareStack_t
{
	unsigned int alloc_idx;
	int stack_size;
	int count;
	stStackMem_t** stack_array;
};

struct stCoRoutine_t
{
	stCoRoutineEnv_t *env;
	pfn_co_routine_t pfn;
	void *arg;
	coctx_t ctx;

	char cStart;
	char cEnd;
	char cIsMain;
	char cEnableSysHook;
	char cIsShareStack;

	uint64_t BaseTime;
	uint64_t CurrentTime;
	uint64_t TargetTime;
	uint64_t ThreadIdCo;

	void *pvEnv;

	stStackMem_t* stack_mem;

	char* stack_sp;
	unsigned int save_size;
	char* save_buffer;

	stCoSpec_t aSpec[1024];

};

void 				co_init_curr_thread_env();
stCoRoutineEnv_t *	co_get_curr_thread_env();

// 拆掉本线程的 env：释放主伪协程、epoll fd、超时时间轮与 env 自身，并把
// gCoEnvPerThread 置回 NULL。必须由本线程在退出前调用 —— env 是 thread_local，
// 别的线程调不到它；线程退出时没有任何析构会替它收这个尾。
// 调用前本线程自己的协程应已用 co_free 释放（那些协程不在 pCallStack 上）。
void 				co_free_curr_thread_env();

void    co_free( stCoRoutine_t * co );
void    co_yield_env(  stCoRoutineEnv_t *env );

struct stTimeout_t;
struct stTimeoutItem_t ;

stTimeout_t *AllocTimeout( int iSize );
void 	FreeTimeout( stTimeout_t *apTimeout );
int  	AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,uint64_t allNow );

struct stCoEpoll_t;
stCoEpoll_t * AllocEpoll();
void 		FreeEpoll( stCoEpoll_t *ctx );

stCoRoutine_t *		GetCurrThreadCo();
void 				SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev );

typedef void (*pfnCoRoutineFunc_t)();

#endif

#define __CO_ROUTINE_INNER_H__
