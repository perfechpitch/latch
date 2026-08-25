#ifndef _LATCH_MEMORY_SYSTEM_
#define _LATCH_MEMORY_SYSTEM_

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>

#include "isa/memory.h"
#include "register.h"
#include "utils.h"

namespace latch {

using systeml::MemoryBase;
using systeml::MemoryPage;
using systeml::MemoryPort;
using systeml::MemoryRouter;

enum class MemKind { ram, rom, device };

class RoutedMemorySystem : public MemoryBase {
 public:
  RoutedMemorySystem() : router(std::make_shared<MemoryRouter>()) {}

  // override 3 参 Read/Write 会隐藏基类模板重载，用 using 拉回。
  using MemoryBase::Read;
  using MemoryBase::Write;

  void AddRegion(uint64_t base, uint64_t size, MemKind kind, uint8_t fill) {
    (void)kind;
    auto store = std::make_shared<MemoryBase>(size, fill);
    router->AddPort(store, base, size, 0, 0, "");
    backings.push_back(store);
    uint64_t end = (size == UINT64_MAX) ? UINT64_MAX : base + size;
    if (end > endAddr) endAddr = end;
  }

  int Read(uint8_t* dataPtr, uint64_t deviceAddr, uint64_t byteWidth) override {
    int r = router->Read(dataPtr, deviceAddr, byteWidth);
    LOGCHECK(r == 0, "访存地址不在任何已声明 memory 区段内(策略 A:严格 fault;0 区段即本架构无存储器)");
    return r;
  }
  int Write(uint8_t* dataPtr, uint64_t deviceAddr, uint64_t byteWidth) override {
    int r = router->Write(dataPtr, deviceAddr, byteWidth);
    LOGCHECK(r == 0, "访存地址不在任何已声明 memory 区段内(策略 A:严格 fault;0 区段即本架构无存储器)");
    return r;
  }
  uint64_t Size() override { return endAddr; }
  void Clear() override {
    for (auto& b : backings) b->Clear();
  }

  std::shared_ptr<MemoryRouter> Router() const { return router; }

 private:
  std::shared_ptr<MemoryRouter> router;
  std::vector<std::shared_ptr<MemoryBase>> backings;
  uint64_t endAddr = 0;
};

}  // namespace latch

#endif
