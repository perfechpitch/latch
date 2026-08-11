#ifndef _LATCH_LOG_
#define _LATCH_LOG_

#ifdef NDEBUG
#undef NDEBUG
#include <assert.h>
#define NDEBUG
#else
#include <assert.h>
#endif

#include <functional>

#include "spdlog/spdlog.h"

extern void __assert_fail(const char* __assertion, const char* __file, unsigned int __line, const char* __function)
    __attribute__((__noreturn__));

namespace latch {

#define LOGCHECK(condition, ...)               \
  __builtin_expect(!!(condition), 1) ? (void)0 \
                                     : __assert_fail(#condition ": " __VA_ARGS__, __FILE__, __LINE__, __FUNCTION__)

}
#endif
