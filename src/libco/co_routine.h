
#ifndef __CO_ROUTINE_H__
#define __CO_ROUTINE_H__

#include <stdint.h>
#include <sys/poll.h>
#include <pthread.h>

struct stCoRoutine_t;
struct stShareStack_t;

struct stCoRoutineAttr_t
{
	int stack_size;
	stShareStack_t*  share_stack;
	stCoRoutineAttr_t()
	{
		stack_size = 128 * 1024;
		share_stack = NULL;
	}
}__attribute__ ((packed));

struct stCoEpoll_t;
typedef int (*pfn_co_eventloop_t)(void *);
typedef void *(*pfn_co_routine_t)( void * );

int 	co_create( stCoRoutine_t **co,const stCoRoutineAttr_t *attr,void *(*routine)(void*),void *arg );
void    co_resume( stCoRoutine_t *co );
void    co_yield_co( stCoRoutine_t *co );
void    co_yield_ct();
void    co_release( stCoRoutine_t *co );
void    co_reset(stCoRoutine_t * co);

stCoRoutine_t *co_self();

int		co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms );
void 	co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg );

int 	co_setspecific( pthread_key_t key, const void *value );
void *	co_getspecific( pthread_key_t key );

stCoEpoll_t * 	co_get_epoll_ct();

void 	co_enable_hook_sys();
void 	co_disable_hook_sys();
bool 	co_is_enable_sys_hook();

struct stCoCond_t;

stCoCond_t *co_cond_alloc();
int co_cond_free( stCoCond_t * cc );

int co_cond_signal( stCoCond_t * );
int co_cond_broadcast( stCoCond_t * );
int co_cond_timedwait( stCoCond_t *,int timeout_ms );

stShareStack_t* co_alloc_sharestack(int iCount, int iStackSize);

void co_set_env_list( const char *name[],size_t cnt);

void co_log_err( const char *fmt,... );
#endif
