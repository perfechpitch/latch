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
#include <cstdio>
#include <cstdlib>

#include "spdlog/spdlog.h"

namespace latch {

[[noreturn]] inline void LogCheckFail(const char* assertion, const char* file,
                                      unsigned int line,
                                      const char* function) {
  std::fprintf(stderr, "%s:%u: %s: assertion failed: %s\n", file, line,
               function, assertion);
  std::abort();
}

#define LOGCHECK(condition, ...)               \
  __builtin_expect(!!(condition), 1) ? (void)0 \
                                     : ::latch::LogCheckFail(               \
                                           #condition ": " __VA_ARGS__,    \
                                           __FILE__, __LINE__, __FUNCTION__)

}
#endif
