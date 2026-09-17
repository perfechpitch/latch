#ifndef _LATCH_BACH_IP_CHIP_CORE_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_PORTS_

// core 内的端口束。
//
// 一个端口组是一组上拍写、下拍读的寄存器。同一个束里不同字段可以有不同的写者
// （req_* 归 master，ready 与 rsp_* 归 slave），因为每个字段各是一个 Latch，
// 「同一拍同一个 Latch 只能一个写者」这条约束是按字段算的。
//
// 协议照各单元文档的接口声明：
//   valid/ready        发送方拉 valid 并保持数据不变直到看见 ready；接收方的
//                      ready 是它上一拍锁存的值，一次握手最少两拍
//   脉冲               单拍有效，接收方当拍锁存
//   电平               持续有效，接收方任意拍读

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/logic.h"

namespace latch {
namespace bach {

using ByteBlock = std::vector<uint8_t>;
using ByteBlockPtr = std::shared_ptr<ByteBlock>;

// 存储的一个 master 端口。
// scale 旁带的三档访问。MXFP8 数据每 32 B 配 1 B scale，与数据地址一一对应。
//   不带         只走 SRAM 那一段
//   正文接 scale 读回来的正文后面接上 [addr, addr + bytes) 覆盖到的那几组 scale；
//                写进来的 wdata 是正文后面接 scale
//   只访问 scale 地址与长度仍按数据的坐标给，读回来、写进去的只有 scale 那几组。
//                DTE 边读边发，包里 scale 排在数据后面，搬 scale 那一笔走这一档
enum ScaleAccess : uint64_t {
  kScaleNone = 0,
  kScaleWithData = 1,
  kScaleOnly = 2,
};

// 一组 scale 管多少字节的数据：MXFP8 32 个元素共用 1 B。
constexpr uint64_t kScaleGroupBytes = 32;

class MemPort : public Logic {
 public:
  // master 侧写
  //
  // req_woff 是这一笔在 wdata 里的有效起点：CM 接口一次固定搬一整块，而一条
  // 向量的两端可能落在块中间，那两头的字节不该被这一笔动。写请求因此除了地址
  // 与长度还要给出块内起点，存储只改 [addr + woff, addr + woff + bytes)。
  Logic64 req_valid, req_we, req_addr, req_bytes, req_scale_en, req_woff;
  LogicPtr<ByteBlock> req_wdata;
  // 每发一笔加一。收方按它认这一笔见没见过：两侧各自打拍时，谁先跑决定读到
  // 的是当拍还是上一拍的值，只看 req_valid 就可能把同一笔收两次或漏掉一次。
  Logic64 req_seq;
  // 存储侧写
  Logic64 req_ready, rsp_valid;
  LogicPtr<ByteBlock> rsp_rdata;

  explicit MemPort(ClockPtr c)
      : req_valid(c), req_we(c), req_addr(c), req_bytes(c), req_scale_en(c),
        req_woff(c), req_wdata(c), req_seq(c), req_ready(c), rsp_valid(c),
        rsp_rdata(c) {
    Fields(req_valid, req_we, req_addr, req_bytes, req_scale_en, req_woff,
           req_wdata, req_seq, req_ready, rsp_valid, rsp_rdata);
  }

  // master 每拍二选一调一次。
  void Read(uint64_t addr, uint64_t bytes, bool scale_en = false) {
    req_valid = 1;
    req_we = 0;
    req_addr = addr;
    req_bytes = bytes;
    req_scale_en = scale_en ? 1 : 0;
    req_woff = 0;
    req_wdata = ByteBlockPtr();
    req_seq = ++issue_seq;
  }
  // woff 与 bytes 给出这一笔在 wdata 里的有效区间。整块写就用默认值。
  void Write(uint64_t addr, ByteBlockPtr data, bool scale_en = false,
             uint64_t woff = 0, uint64_t bytes = 0) {
    req_valid = 1;
    req_we = 1;
    req_addr = addr;
    req_bytes = bytes != 0 ? bytes : (data ? data->size() - woff : 0);
    req_scale_en = scale_en ? 1 : 0;
    req_woff = woff;
    req_wdata = data;
    req_seq = ++issue_seq;
  }
  // 只读 scale：bytes 是数据那一侧的长度，读回 ceil(bytes / 32) 个 scale。
  void ReadScale(uint64_t addr, uint64_t bytes) {
    Read(addr, bytes);
    req_scale_en = kScaleOnly;
  }
  // 只写 scale：scale 里第 k 个落在 addr + k × 32 那一组，bytes 是数据那一侧
  // 覆盖的长度。
  void WriteScale(uint64_t addr, ByteBlockPtr scale, uint64_t bytes) {
    Write(addr, std::move(scale), false, 0, bytes);
    req_scale_en = kScaleOnly;
  }
  void IdleReq() {
    req_valid = 0;
    req_we = 0;
    req_addr = 0;
    req_bytes = 0;
    req_scale_en = 0;
    req_woff = 0;
    req_wdata = ByteBlockPtr();
    req_seq = req_seq.Get();
  }

  // 存储每拍调一次。
  void DriveSlave(bool ready, bool rsp, ByteBlockPtr data) {
    req_ready = ready ? 1 : 0;
    rsp_valid = rsp ? 1 : 0;
    rsp_rdata = std::move(data);
  }

  uint64_t Seq() const { return req_seq.Get(); }
  bool Ready() const { return req_ready.Get() != 0; }
  bool RspValid() const { return rsp_valid.Get() != 0; }
  ByteBlockPtr RspData() const { return rsp_rdata.Get(); }

 private:
  // master 自己的计数，不跨模块读，所以是普通成员。
  uint64_t issue_seq = 0;
};

struct MemReqView {
  bool valid = false;
  bool we = false;
  uint64_t addr = 0;
  uint64_t bytes = 0;
  bool scale_en = false;      // 正文接 scale
  bool scale_only = false;    // 只访问 scale
  uint64_t woff = 0;
  uint64_t seq = 0;
  ByteBlockPtr wdata;
};

// 把一笔收下来的请求原样发到另一个口上。DTE 的 DMA_XBAR 转发走这一条。
inline void ForwardMemReq(MemPort& p, MemReqView const& r) {
  if (r.we) {
    if (r.scale_only) {
      p.WriteScale(r.addr, r.wdata, r.bytes);
    } else {
      p.Write(r.addr, r.wdata, r.scale_en, r.woff, r.bytes);
    }
  } else if (r.scale_only) {
    p.ReadScale(r.addr, r.bytes);
  } else {
    p.Read(r.addr, r.bytes, r.scale_en);
  }
}

inline MemReqView ReadMemReq(MemPort const& p) {
  MemReqView v;
  v.valid = p.req_valid.Get() != 0;
  if (!v.valid) return v;
  v.we = p.req_we.Get() != 0;
  v.addr = p.req_addr.Get();
  v.bytes = p.req_bytes.Get();
  uint64_t scale = p.req_scale_en.Get();
  v.scale_en = scale == kScaleWithData;
  v.scale_only = scale == kScaleOnly;
  v.woff = p.req_woff.Get();
  v.seq = p.req_seq.Get();
  v.wdata = p.req_wdata.Get();
  return v;
}

}  // namespace bach
}  // namespace latch

#endif
