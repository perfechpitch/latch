#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_CUSTOM0_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_CUSTOM0_

// custom-0 自定义指令的模型侧译码与执行（《RV Core自定义指令详细设计》的 6 条）。
//
// src/rv32 的功能模型只认标准 RV32IMC，解码表由 gen/ codegen 出来，不宜手改。
// 自定义指令按 rv-core 设计的接入点接在 rv_core 这一层：SystemRv32Bach 覆盖
// SystemRv32::Decode，opcode = 0x0B（低 2 位为 11，与压缩指令必然错开）的一律
// 译成本文件里的指令对象，其余转回基类。
//
// 字段布局与软件侧的 self_inst.h（src/bach/compiler/kernel/）一字不差，改一处
// 要一起改：
//   dsaw   funct3=001, bit31=0：数据 = rs1，DSA 字节地址 = rs2
//   dsawi  funct3=001, bit31=1：数据 = rs1，imm16 = bits[30:20] ∪ bits[11:7]
//   dsar   funct3=000, bit31=0：DSA 字节地址 = rs1，结果写 rd
//   dsari  funct3=000, bit31=1：imm16 = bits[30:15]，结果写 rd
//   task_done funct3=010, bits[29:25]=0：bit31 是 TS 标志
//   loop   funct3=110，标准 B 型立即数：rs1 = 最大次数，rs2 = 当前次数
//
// 行为复用 MMIO 那条通路：dsaw/dsawi/dsar/dsari 对 DSA IO 窗口、task_done 对
// task 控制区各做一笔标量读写，exec.h 的 DrainSinks 把 MmioSink 里排上的请求
// 转成端口动作——与 kernel 现在用普通 store/load 走约定地址完全等价，gpr 就绪
// 表、dsa_rq 写回、task_queue 交还这些机制一概不动。loop 不访存，改 LocalPc
// 并 MarkBranched，由 Engine 的 StepContext 按「分支成立取 LocalPc」收尾。
//
// DSA IO 窗口基址按核不同（DTE / MU / VU），Decode 那一刻由 SystemRv32Bach
// 注进指令对象；指令体里只拿 System 基类的标量访存原语，不回头认子类。

#include <cstdint>
#include <memory>

#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "isa/instruction.h"
#include "isa/instance.h"
#include "isa/program.h"
#include "rv32/rv32.h"

namespace latch {
namespace bach {

// 6 条指令公用的字段抽取。构造时从 32 位指令字里拆出来，RunOnInstance 只读
// 拆好的值，不在执行路径上重复移位。
struct Custom0Fields {
  uint32_t rs1 = 0, rs2 = 0, rd = 0;

  explicit Custom0Fields(uint32_t w)
      : rs1((w >> 15) & 0x1Fu), rs2((w >> 20) & 0x1Fu), rd((w >> 7) & 0x1Fu) {}

  // dsawi 的 16 bit 立即数：imm[15:10] 在 funct7[5:0]，imm[9:5] 在 rs2 字段，
  // imm[4:0] 在 rd 字段（self_inst.h 的 dsawi 宏按这个布局注入）。
  static uint32_t ImmW(uint32_t w) {
    return (((w >> 25) & 0x3Fu) << 10) | (((w >> 20) & 0x1Fu) << 5) |
           ((w >> 7) & 0x1Fu);
  }
  // dsari 的 16 bit 立即数：imm[15:10] 在 funct7[5:0]，imm[9:5] 在 rs2 字段，
  // imm[4:0] 在 rs1 字段。
  static uint32_t ImmR(uint32_t w) {
    return (((w >> 25) & 0x3Fu) << 10) | (((w >> 20) & 0x1Fu) << 5) |
           ((w >> 15) & 0x1Fu);
  }
  // loop 的 B 型立即数，重建出有符号字节偏移：
  // imm[12]=bit31、imm[10:5]=bits[30:25]、imm[4:1]=bits[11:8]、imm[11]=bit7。
  static int32_t ImmB(uint32_t w) {
    uint32_t imm13 = (((w >> 31) & 1u) << 12) | (((w >> 7) & 1u) << 11) |
                     (((w >> 25) & 0x3Fu) << 5) | (((w >> 8) & 0xFu) << 1);
    return int32_t(imm13) - ((imm13 & 0x1000u) ? 0x2000 : 0);
  }
};

// 功能模型上挂的自定义指令。RunOnInstance 里拿 InstanceRv32 的 SR 快照读写
// 寄存器，访存走 GetSystem() 的标量原语（isa/system.h）。
class Custom0Inst : public Instruction {
 public:
  Custom0Inst(const char* name, uint32_t w, uint64_t base)
      : Instruction(name, 32), dsa_base(base), word(w), f(word) {}
  ~Custom0Inst() override = default;

 protected:
  // 本核绑定的 DSA 的 IO 窗口基址，DSA 寄存器地址都是窗口内偏移。
  uint64_t dsa_base;
  uint32_t word;
  Custom0Fields f;
};

// 寄存器寻址写 DSA 单寄存器：数据 = rs1，地址 = rs2。
class Custom0Dsaw : public Custom0Inst {
 public:
  Custom0Dsaw(uint32_t w, uint64_t base) : Custom0Inst("dsaw", w, base) {}
  void RunOnInstance(Instance* instance) override {
    auto* inst = static_cast<rv32::InstanceRv32*>(instance);
    instance->GetSystem()->StoreMem<uint32_t>(
        dsa_base + inst->LocalSr(f.rs2), inst->LocalSr(f.rs1));
  }
};

// 立即数寻址写 DSA 单寄存器：数据 = rs1，地址 = imm16。
class Custom0Dsawi : public Custom0Inst {
 public:
  Custom0Dsawi(uint32_t w, uint64_t base) : Custom0Inst("dsawi", w, base) {}
  void RunOnInstance(Instance* instance) override {
    auto* inst = static_cast<rv32::InstanceRv32*>(instance);
    instance->GetSystem()->StoreMem<uint32_t>(
        dsa_base + Custom0Fields::ImmW(word), inst->LocalSr(f.rs1));
  }
};

// 寄存器寻址读 DSA 单寄存器：地址 = rs1，结果写 rd。
//
// 功能模型这一拍拿到的是占位值（MmioSink 同步返回上次写进去的值），真正的
// 内容由 dsa_wb 异步补进 rd；就绪位由 exec.h 的 DrainSinks 清、写回时立。
class Custom0Dsar : public Custom0Inst {
 public:
  Custom0Dsar(uint32_t w, uint64_t base) : Custom0Inst("dsar", w, base) {}
  void RunOnInstance(Instance* instance) override {
    auto* inst = static_cast<rv32::InstanceRv32*>(instance);
    uint32_t addr = inst->LocalSr(f.rs1);
    inst->SetLocalSr(f.rd, instance->GetSystem()->LoadMem<uint32_t>(
                               dsa_base + addr));
  }
};

// 立即数寻址读 DSA 单寄存器：地址 = imm16，结果写 rd。
class Custom0Dsari : public Custom0Inst {
 public:
  Custom0Dsari(uint32_t w, uint64_t base) : Custom0Inst("dsari", w, base) {}
  void RunOnInstance(Instance* instance) override {
    auto* inst = static_cast<rv32::InstanceRv32*>(instance);
    inst->SetLocalSr(f.rd, instance->GetSystem()->LoadMem<uint32_t>(
                               dsa_base + Custom0Fields::ImmR(word)));
  }
};

// task 完成标志：往 task 控制区的 task_done 口写 TS。写 1 通知 TS、写 0 只交
// 还自己，语义与 kernel 现在 mmio_write(TASK_CTRL_BASE, TC_TASK_DONE, x) 相同，
// 后续动作（finish/done 端口、停机、跳队头 task）都在 HandleTaskCtrl 里。
class Custom0TaskDone : public Custom0Inst {
 public:
  Custom0TaskDone(uint32_t w) : Custom0Inst("task_done", w, 0) {}
  void RunOnInstance(Instance* instance) override {
    instance->GetSystem()->StoreMem<uint32_t>(kTaskCtrlBase + kRegTaskDone,
                                              (word >> 31) & 1u);
  }
};

// 自定义循环分支：rs1 = 最大次数，rs2 = 当前次数。rs2 < rs1 跳回 PC + offset，
// 否则顺序执行。offset 是 B 型布局的有符号字节偏移（低 1 bit 隐含为 0）。
class Custom0Loop : public Custom0Inst {
 public:
  Custom0Loop(uint32_t w) : Custom0Inst("loop", w, 0) {}
  void RunOnInstance(Instance* instance) override {
    auto* inst = static_cast<rv32::InstanceRv32*>(instance);
    uint32_t cur = inst->LocalSr(f.rs2), max = inst->LocalSr(f.rs1);
    if (cur < max) {
      instance->LocalPc() =
          uint32_t(int32_t(instance->LocalPc()) + Custom0Fields::ImmB(word));
      instance->MarkBranched();
    }
  }
};

// RV core 的功能模型系统：在标准 RV32IMC 之上接入 custom-0。dsa_base 是本核
// 绑定的 DSA 的 IO 窗口基址（DTE / MU / VU 各不同），自定义指令的地址都是
// 窗口内偏移，Decode 时由这里补全成绝对地址再交给指令体。
class SystemRv32Bach : public rv32::SystemRv32 {
 public:
  SystemRv32Bach(std::shared_ptr<ISA> isa, uint64_t pc, uint32_t thread,
                 uint64_t dsa_base)
      : SystemRv32(isa, pc, thread), dsa_io_base(dsa_base) {}

  std::shared_ptr<Instruction> Decode(
      std::shared_ptr<InstBinary> instPkg) const override {
    uint32_t w = uint32_t(instPkg->GetBinary64());
    if ((w & 0x7Fu) == 0x0Bu) {
      uint32_t funct3 = (w >> 12) & 0x7u;
      bool imm = (w >> 31) & 1u;
      switch (funct3) {
        case 0:   // dsar / dsari
          if (imm) return std::make_shared<Custom0Dsari>(w, dsa_io_base);
          return std::make_shared<Custom0Dsar>(w, dsa_io_base);
        case 1:   // dsaw / dsawi
          if (imm) return std::make_shared<Custom0Dsawi>(w, dsa_io_base);
          return std::make_shared<Custom0Dsaw>(w, dsa_io_base);
        case 2:   // task_done（bits[29:25] == 0；00001 那档是 flag_check，不在
          // 本设计的 6 条之内，回落基类按非法指令处理）
          if (((w >> 25) & 0x1Fu) == 0)
            return std::make_shared<Custom0TaskDone>(w);
          break;
        case 6:   // loop
          return std::make_shared<Custom0Loop>(w);
        default:
          break;
      }
    }
    return SystemRv32::Decode(instPkg);
  }

 private:
  uint64_t dsa_io_base;
};

}  // namespace bach
}  // namespace latch

#endif
