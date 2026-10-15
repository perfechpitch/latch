#include <gtest/gtest.h>

#include "bach/ip/chip/core/dte/regfile.h"

using namespace latch::bach;

TEST(DteCompat, PcodeTransferModeRetainsTopkEnable) {
  auto old_shift = DteSegValidShift();
  DteSegValidShift() = kDteSegValidShiftPcode;
  DteConfig cfg;
  cfg.SetField(18, 793); // MM receive, topK+data, ShareMem flag, TS ACK.
  EXPECT_EQ(cfg.trans_mode, 793u);
  EXPECT_EQ((cfg.trans_mode >> DteSegValidShift()) & 7, 3u);
  cfg.SetField(18, 0x3ff);
  EXPECT_EQ(cfg.trans_mode, 0x3bfu); // Pcode bit6 remains reserved.
  DteSegValidShift() = kDteSegValidShift;
  cfg.SetField(18, 0x3ff);
  EXPECT_EQ(cfg.trans_mode, 0x3f7u); // Legacy bit3 remains reserved.
  DteSegValidShift() = old_shift;
}
