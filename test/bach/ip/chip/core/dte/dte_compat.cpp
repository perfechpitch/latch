#include <gtest/gtest.h>

#include "bach/ip/chip/core/dte/regfile.h"
#include "bach/ip/chip/core/dte/hmem.h"

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

TEST(DteCompat, PcodeHeaderReadReturnsMaskInLowBitsForEveryStream) {
  latch::RT::Reset(8,8);
  auto old=DtePcodeFormat();DtePcodeFormat()=true;
  {
    auto clock=latch::MakeClock(0,1);
    Agcu agcu;
    DteRegfile reg(clock,"reg",agcu,0,false);
    Hmem table(clock,"header",0,false);
    reg.BindControl([&table](uint64_t address) { return table.ReadSoftware(address); },
                    [&table](uint64_t address, uint64_t data) { table.WriteSoftware(address, data); });
    for(uint64_t sid=0;sid<16;++sid) table.Entry(sid).core_mask=(sid+1)*3;
    for(uint64_t sid=0;sid<16;++sid)
      EXPECT_EQ(reg.ConfigRead(0x3200+sid*32),(sid+1)*3);
    EXPECT_EQ(reg.ConfigRead(0x3004),0u);
  }
  DtePcodeFormat()=old;latch::RT::Reset();
}

TEST(DteCompat, PcodeHeaderStrideSelectsDistinctNativeStreamGroups) {
  latch::RT::Reset(8,8);
  auto old=DtePcodeFormat();DtePcodeFormat()=true;
  {
    auto clock=latch::MakeClock(0,1);
    Agcu agcu;
    Hmem table(clock,"header",0,false);
    for(uint64_t sid=0;sid<16;++sid) {
      Segment header;header.dst=0x4802b000;header.stride=32;
      agcu.ExpandOut(header,sid,Route::kRouterToCm);
      EXPECT_EQ(header.dst_addr,sid*32);
      HmemEntry entry;entry.core_mask=sid+1;
      table.WriteIndexed(header.dst_addr,HeaderToBytes(entry));
    }
    for(uint64_t sid=0;sid<16;++sid)
      EXPECT_EQ(table.Entry(sid).core_mask,sid+1);
  }
  DtePcodeFormat()=old;latch::RT::Reset();
}
