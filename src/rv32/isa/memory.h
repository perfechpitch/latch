#ifndef _LATCH_ISA_MEMORY_
#define _LATCH_ISA_MEMORY_

// isa 级访存抽象。合并自原 base/memory_base.h + base/address_router.h +
// isa/memory_router.h（memory_base 依赖 isa/utils.h，本就不属于最底层 base/）：
//   - MemoryPage / MemoryPort / MemoryBase —— 分页内存与读写端口基类
//   - AddressSpace / AddressRouter<T>       —— 按地址区间路由的通用块
//   - MemoryRouter                          —— 包 AddressRouter<MemoryPort>，按地址派发 Read/Write
// 全部 namespace systeml；消费者的 `using systeml::MemoryBase / MemoryRouter;` 不用改。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "isa/utils.h"  // LOGCHECK / Int2Str / spdlog

namespace systeml {

// ===== 分页内存 + 读写端口基类（原 base/memory_base.h）=====

class MemoryPage {
 public:
  MemoryPage(uint64_t size, uint8_t fill = 0x55) : Size(size), addrBase(0) {
    rawData = (uint8_t*)std::malloc(size);
    std::memset(rawData, fill, size);
  }

  MemoryPage(uint64_t size, uint64_t addrbase)
      : Size(size), addrBase(addrbase) {
    rawData = (uint8_t*)std::malloc(size);
    std::memset(rawData, 0x55, size);
  }
  ~MemoryPage() { free(rawData); }

  void Reset(uint8_t value) { std::memset(rawData, value, Size); }

  const uint8_t* Read(uint64_t addr, uint64_t size) const {
    LOGCHECK(addr >= addrBase, "");
    LOGCHECK((addr - addrBase + size) <= Size, "Read ERROR!");
    return rawData + addr - addrBase;
  }

  uint64_t ReadUint(uint64_t addr, uint64_t size) const {
    auto offset = Read(addr, size);
    if (size == 1) return (uint64_t)(*((uint8_t*)offset));
    if (size == 2) return (uint64_t)(*((uint16_t*)offset));
    if (size == 4) return (uint64_t)(*((uint32_t*)offset));
    if (size == 8) return (uint64_t)(*((uint64_t*)offset));
    LOGCHECK(false, "");
    return 0;
  }

  int Read(uint8_t* dataPtr, uint64_t addr, uint64_t byteSize) const {
    LOGCHECK(addr >= addrBase, "");
    LOGCHECK((addr - addrBase + byteSize) <= Size, "Read ERROR!");
    std::memcpy(dataPtr, rawData + addr - addrBase, byteSize);
    return 0;
  }

  int Write(uint8_t* data, uint64_t addr, uint64_t size) {
    LOGCHECK(addr >= addrBase, "");
    LOGCHECK((addr - addrBase + size) <= Size, "Write ERROR!");
    std::memcpy(rawData + addr - addrBase, data, size);
    return 0;
  }

  uint8_t* GetRawAddr() { return rawData; }

 private:
  uint64_t Size;
  uint8_t* rawData;
  uint64_t addrBase;
};

class MemoryPort {
 public:
  virtual uint64_t Size() = 0;
  virtual int Read(uint8_t* dataPtr, uint64_t deviceAddr,
                   uint64_t byteWidth) = 0;
  virtual int Write(uint8_t* dataPtr, uint64_t deviceAddr,
                    uint64_t byteWidth) = 0;
};

class MemoryBase : public MemoryPort {
 public:
  static const uint64_t memPageSize = 2 * 1024 * 1024;

  MemoryBase(uint64_t size = UINT64_MAX, uint8_t fill = 0x55) : addrSize(size), fillByte(fill) {}

  ~MemoryBase() {
    for (auto pageItem : memPageTable) {
      delete pageItem.second;
    }
  }

  virtual void Clear() {
    for (auto pageItem : memPageTable) {
      delete pageItem.second;
    }
    memPageTable.clear();
  }

  virtual uint64_t Size() { return addrSize; }

  virtual int Read(uint8_t* dataPtr, uint64_t deviceAddr,
                   uint64_t byteWidth) override {
    auto info = SplitToPages(deviceAddr, byteWidth);
    uint64_t offset = 0;
    for (auto i : info) {
      auto mem = GetMemoryPage(i[0])->GetRawAddr() + i[1];
      std::memcpy(dataPtr + offset, mem, i[2]);
      offset += i[2];
    }
    return 0;
  }

  template <typename T>
  std::vector<T> Read(uint64_t deviceAddr, uint64_t size) {
    std::vector<T> value(size);
    Read((uint8_t*)(value.data()), deviceAddr, size * sizeof(T));
    return value;
  }

  template <typename T>
  T Read(uint64_t deviceAddr) {
    T value;
    Read((uint8_t*)(&value), deviceAddr, sizeof(T));
    return value;
  }

  virtual int Write(uint8_t* dataPtr, uint64_t deviceAddr,
                    uint64_t byteWidth) override {
    uint8_t* pa = getRaw(deviceAddr);
    uint64_t memByte;
    memByte = byteWidth;
    auto info = SplitToPages(deviceAddr, byteWidth);
    uint64_t offset = 0;
    for (auto i : info) {
      auto mem = GetMemoryPage(i[0])->GetRawAddr() + i[1];
      std::memcpy(mem, dataPtr + offset, i[2]);
      offset += i[2];
    }
    CheckMemWatchPoint(pa, deviceAddr, memByte);
    return 0;
  }

  template <typename T>
  int Write(uint64_t deviceAddr, const std::vector<T>& value) {
    if (value.empty()) return 0;
    auto ptr = (uint8_t*)(value.data());
    Write(ptr, deviceAddr, value.size() * sizeof(T));
    return 0;
  }

  template <typename T>
  int Write(uint64_t deviceAddr, T value) {
    auto ptr = (uint8_t*)(&value);
    Write(ptr, deviceAddr, sizeof(T));
    return 0;
  }

  void AddWatchPoint(uint64_t addr) {
    memWatchPoint.insert(
        std::pair<uint64_t, std::vector<std::string>>(addr, {}));
  }

  std::vector<std::string> GetMemWatchPoint(uint64_t addr) {
    return memWatchPoint[addr];
  }

  void CheckMemWatchPoint(uint8_t* pa, uint64_t deviceAddr, uint64_t memByte) {
    if (memWatchPoint.find(deviceAddr) != memWatchPoint.end()) {
      std::string data = "";
      for (uint64_t i = 0; i < memByte; i++) {
        data = data + Int2Str(*(pa + i));
      }
      memWatchPoint[deviceAddr].push_back(data);
    }
  }

  std::string ToStr(uint64_t startAddr, uint64_t byteWdith) {
    uint8_t* pa = getRaw(startAddr);
    std::string results = "hex: ";
    for (uint64_t i = 0; i < byteWdith; i++) {
      results = results + Int2Str(*(pa + i));
    }
    return results;
  }

 protected:
  uint64_t GetPageIdx(uint64_t addr) {
    return addr >> uint64_t(std::log2(memPageSize));
  }

  uint64_t GetPageOffset(uint64_t deviceAddr) {
    uint64_t offset = 0;
    for (uint64_t i = 0; i < uint64_t(std::log2(memPageSize)); i++) {
      offset = offset | ((uint64_t)1 << i);
    }
    return deviceAddr & offset;
  }

  std::vector<std::vector<uint64_t>> SplitToPages(uint64_t startAddr,
                                                  uint64_t size) {
    std::vector<std::vector<uint64_t>> splitInfo;
    auto remain = size;
    auto start = startAddr;
    while (remain > 0) {
      uint64_t pageIdx = GetPageIdx(start);
      auto offset = GetPageOffset(start);
      if ((memPageSize - offset) >= remain) {
        splitInfo.push_back({pageIdx, offset, remain});
        remain = 0;
      } else {
        splitInfo.push_back({pageIdx, offset, memPageSize - offset});
        remain = remain - (memPageSize - offset);
        start = start + (memPageSize - offset);
      }
    }
    return splitInfo;
  }

  MemoryPage* GetMemoryPage(uint64_t pageIdx) {
    if (memPageTable.find(pageIdx) != memPageTable.end()) {
      return memPageTable[pageIdx];
    } else {
      MemoryPage* page = new MemoryPage(memPageSize, fillByte);
      memPageTable[pageIdx] = page;
      return page;
    }
  }

  uint8_t* getRaw(uint64_t deviceAddr) {
    uint64_t pageIdx = GetPageIdx(deviceAddr);
    MemoryPage* page = GetMemoryPage(pageIdx);
    uint8_t* pa = page->GetRawAddr() + GetPageOffset(deviceAddr);
    return pa;
  }

  uint64_t addrSize;
  uint8_t fillByte = 0x55;
  std::map<uint64_t, MemoryPage*> memPageTable;
  std::map<uint64_t, std::vector<std::string>> memWatchPoint;
};

// ===== 按地址区间路由的通用块（原 base/address_router.h）=====

template <typename T>
struct AddressSpace {
  uint64_t base;
  uint64_t size;
  int64_t offset;
  int priority;
  std::shared_ptr<T> object;
  std::string name;

  bool matches(uint64_t addr) const {
    return addr >= base && addr < (base + size);
  }
};

template <typename T>
class AddressRouter {
 private:
  std::vector<AddressSpace<T>> rangeSpaces;

  void sort_spaces() {
    std::sort(rangeSpaces.begin(), rangeSpaces.end(),
              [](const AddressSpace<T>& a, const AddressSpace<T>& b) {
                return a.base < b.base;
              });
  }

 public:
  AddressRouter() : rangeSpaces() {}

  void Reset() { rangeSpaces.clear(); }

  AddressSpace<T> MapPort(uint64_t addr) const {
    int bestIndex = -1;
    int max_priority = -1;

    if (!rangeSpaces.empty()) {
      int left = 0, right = rangeSpaces.size() - 1;
      int candidate_idx = -1;
      while (left <= right) {
        int mid = (left + right) / 2;
        if (rangeSpaces[mid].base <= addr) {
          candidate_idx = mid;
          left = mid + 1;
        } else {
          right = mid - 1;
        }
      }
      if (candidate_idx != -1) {
        for (int i = candidate_idx; i >= 0; --i) {
          const auto& space = rangeSpaces[i];
          if (space.matches(addr)) {
            if (space.priority > max_priority) {
              max_priority = space.priority;
              bestIndex = i;
            }
            if (space.base + space.size <= addr) {
              break;
            }
          }
        }
      }
    }
    if (bestIndex != -1)
      return rangeSpaces.at(bestIndex);
    else
      return AddressSpace<T>({0, 0, 0, 0, nullptr, ""});
  }

  void AddPort(const std::shared_ptr<T>& port, uint64_t start, uint64_t size,
               int priority = 0, int64_t offset = 0,
               std::string portName = "") {
    AddPort({start, size, offset, priority, port, portName});
  }

  void AddPort(const AddressSpace<T>& port) {
    rangeSpaces.push_back(port);
    sort_spaces();
  }
};

// ===== 内存地址路由器：包 AddressRouter<MemoryPort>（原 isa/memory_router.h）=====
// isa 级访存路由，和 RoutedMemorySystem（isa/memory_system.h）同层。

class MemoryRouter : public MemoryPort {
 private:
  AddressRouter<MemoryPort> router;

 public:
  MemoryRouter(uint64_t size = UINT64_MAX) : addrSize(size), router() {}

  void AddPort(std::shared_ptr<MemoryPort> port, uint64_t start, uint64_t size,
               int priority = 0, int64_t offset = 0,
               std::string portName = "") {
    router.AddPort(port, start, size, priority, offset, portName);
  }

  void Reset() { router.Reset(); }

  virtual uint64_t Size() { return addrSize; }

  virtual int Read(uint8_t* dataPtr, uint64_t deviceAddr,
                   uint64_t byteWidth) override {
    auto port = router.MapPort(deviceAddr);
    if (port.object == nullptr) {
      spdlog::error("deviceAddr not mapping : {:#X}", deviceAddr);
      return -1;
    }
    return port.object->Read(dataPtr, deviceAddr + port.offset, byteWidth);
  }
  virtual int Write(uint8_t* dataPtr, uint64_t deviceAddr,
                    uint64_t byteWidth) override {
    auto port = router.MapPort(deviceAddr);
    if (port.object == nullptr) {
      spdlog::error("deviceAddr not mapping : {:#X}", deviceAddr);
      return -1;
    }
    return port.object->Write(dataPtr, deviceAddr + port.offset, byteWidth);
  }
 private:
  uint64_t addrSize;
};

}  // namespace systeml

#endif
