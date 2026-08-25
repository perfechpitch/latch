#ifndef _LATCH_ISA_DECODER_
#define _LATCH_ISA_DECODER_

#include <memory>

#include "base/log.h"
#include "isa/program.h"  // InstBinary

namespace latch {

class Instruction;

// 表驱动的位判定树解码，与 ISA 无关；两份数据由各 ISA 的 codegen 生成：
//   table —— 扁平化的判定树，每个节点占 4 个 int：
//     [0] 取指令的哪一位来分叉（-1 表示已到叶子）
//     [1] / [2] 该位为 0 / 1 时跳转到的节点下标
//     [3] 叶子节点对应的 jumpTable 下标
//   jumpTable —— 第 k 个叶子的指令构造函数。
using DecodeLeafFn = std::shared_ptr<Instruction> (*)(InstBinary);

inline std::shared_ptr<Instruction> DecodeByBitTable(const std::shared_ptr<InstBinary>& pkg, const int* table,
                                                     const DecodeLeafFn* jumpTable, uint32_t maxByteWidth) {
  InstBinary dataBin = pkg->GetSubBinary(0, maxByteWidth);
  uint64_t bin = pkg->GetBinary64();
  uint32_t tableIndex = 0;

  for (uint32_t i = 0; i < maxByteWidth * 8; i++) {
    int encodeIndex = table[tableIndex + 0];
    int value0Offset = table[tableIndex + 1];
    int value1Offset = table[tableIndex + 2];
    int functionIndex = table[tableIndex + 3];

    if (encodeIndex != -1) {
      tableIndex = ((bin >> encodeIndex) & 0x1) ? value1Offset : value0Offset;
    } else {
      return jumpTable[functionIndex](dataBin);
    }
  }

  LOGCHECK(false, "Cannot parse this instruction.");
  return nullptr;
}

}  // namespace latch

#endif
