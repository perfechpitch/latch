#ifndef _LATCH_MODULE_CPU_H_
#define _LATCH_MODULE_CPU_H_

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <vector>

#include "base/time_stamp.h"

namespace latch {

enum class ThdArchState { IDEL, FETCH_INST, EXECUTE_INST, FINISH_INST, EXEC_FINISH };

class Breakpoint {
 public:
  int type;
  uint64_t addr;
  uint64_t len;
};

class CpuBase {
 public:
  CpuBase(uint64_t id, uint32_t threadNum) : cpu_id(id), thread_num(threadNum), breakpoint(threadNum) {}
  ~CpuBase() {}

  virtual void Launch() = 0;

  virtual void Launch(uint32_t tid) = 0;

  virtual void Launch(uint64_t pc, uint32_t tid) = 0;

  virtual void Halt(uint32_t tid) = 0;

  virtual void Halt() = 0;

  virtual bool IsAllThreadHalted() = 0;

  virtual bool IsStHalted(uint32_t tid) = 0;

  virtual uint64_t ReadSr(uint64_t sr_idx, uint32_t tid) = 0;

  virtual void WriteSr(uint64_t sr_idx, uint64_t val, uint32_t tid) = 0;

  virtual uint64_t ReadSpr(uint64_t spr_idx, uint32_t tid) = 0;

  virtual void WriteSpr(uint64_t spr_idx, uint64_t val, uint32_t tid) = 0;

  virtual void EnableExecLog(std::string fileName) = 0;

  void BreakpointRemoveAll(uint32_t tid) { breakpoint[tid].clear(); }
  void BreakpointInsert(int type, uint64_t addr, uint64_t len, uint32_t tid) {
    breakpoint[tid][addr] = {type, addr, len};
  }
  void BreakpointRemove(int type, uint64_t addr, uint64_t len, uint32_t tid) {
    if (breakpoint[tid].find(addr) != breakpoint[tid].end()) {
      breakpoint[tid].erase(addr);
    }
  }
  bool BreakpointHit(uint64_t addr, uint32_t tid) {
    if (breakpoint[tid].find(addr) != breakpoint[tid].end()) {
      return true;
    }
    return false;
  }

 public:
  uint64_t cpu_id;
  uint32_t thread_num;
  std::vector<std::map<uint64_t, Breakpoint>> breakpoint;
};

}
#endif
