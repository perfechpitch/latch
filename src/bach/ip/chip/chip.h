#ifndef _LATCH_BACH_IP_CHIP_CHIP_
#define _LATCH_BACH_IP_CHIP_CHIP_

// 一块 chip：一个核阵列，加上阵列内部的 mesh 连线。
//
// 片内只有四个方向口参与连线，每个核往东与往南各接一条，反向口由对端接回来。边缘上
// 没有邻居的方向不接，所以片的四条边天然是断开的，跨 chip 只能走 PCIe 口。
//
// 它不持有核，也不驱动核：核自己挂时钟，各跑各的协程。这一层管的是"谁跟谁有线"，
// 核在阵列里的位置由它记着，接线时按位置取。
//
// 驱动粒度上提的挂载点也在这里：若核级驱动在大规模 Map 上每拍开销过大，把驱动改到
// 这一层、由它顺序推进各核，连线与核的代码都不用动。

#include <cstdint>
#include <vector>

#include "base/log.h"
#include "bach/common/packet.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/wiring.h"
#include "bach/tables/loader.h"

namespace latch {
namespace bach {

class Chip {
 public:
  Chip(Dim const& d, uint32_t id) : dim(d), chip_id(id) {
    cores.assign(static_cast<size_t>(dim.core_rows_per_chip) *
                     dim.core_cols_per_chip,
                 nullptr);
  }

  uint32_t Id() const { return chip_id; }
  uint32_t Rows() const { return dim.core_rows_per_chip; }
  uint32_t Cols() const { return dim.core_cols_per_chip; }

  void Place(uint32_t local_row, uint32_t local_col, Core* core) {
    LOGCHECK(core != nullptr, "Chip: core is null.");
    Core*& slot = Slot(local_row, local_col);
    LOGCHECK(slot == nullptr, "Chip: that position is already taken.");
    slot = core;
  }

  Core* At(uint32_t local_row, uint32_t local_col) const {
    return Slot(local_row, local_col);
  }

  // 片内 mesh。每个核只主动往东与往南接，避免同一条线被接两次。
  void WireMesh() {
    for (uint32_t r = 0; r < Rows(); ++r) {
      for (uint32_t c = 0; c < Cols(); ++c) {
        Core* here = Slot(r, c);
        LOGCHECK(here != nullptr, "Chip: a position was left empty.");
        if (c + 1 < Cols()) {
          AttachLink(here->Rt(), Port::kEast, Slot(r, c + 1)->Rt());
        }
        if (r + 1 < Rows()) {
          AttachLink(here->Rt(), Port::kSouth, Slot(r + 1, c)->Rt());
        }
      }
    }
  }

 private:
  Core*& Slot(uint32_t r, uint32_t c) {
    LOGCHECK(r < Rows() && c < Cols(), "Chip: position out of range.");
    return cores[static_cast<size_t>(r) * Cols() + c];
  }

  Core* Slot(uint32_t r, uint32_t c) const {
    LOGCHECK(r < Rows() && c < Cols(), "Chip: position out of range.");
    return cores[static_cast<size_t>(r) * Cols() + c];
  }

  Dim dim;
  uint32_t chip_id;
  std::vector<Core*> cores;
};

}
}

#endif
