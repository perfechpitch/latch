#ifndef _LATCH_BACH_IP_WIRING_
#define _LATCH_BACH_IP_WIRING_

// 接一条物理链路。
//
// 一条线是双向的，两端各开一个入口：a 从 port 发出去的包落到 b 的反向口，b 从反向口
// 发出去的包落到 a 的 port。每条线每个方向一个入口，因为入口是单 producer 单 consumer。
//
// 带宽与延迟登记在两端各自的出口上。给 0 表示这条线上没有特别的取值，按端口类型取
// 全局默认：NoC 口用 NoC 带宽，PCIe 口用 PCIe 带宽，延迟按跨 chip 还是跨 node 算。
//
// 接到 PCIe 交换节点上的那种线两端不对称：核那一头是十二方向之一，交换节点那一头是
// 一个按名字开出来的口。核发出去的包落到交换节点那个口的入口，交换节点从那个口发出
// 来的包落回核的同一个方向口。

#include <cstdint>

#include "bach/common/packet.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/node/pcie_switch.h"

namespace latch {
namespace bach {

inline void AttachLink(Router& a, Port port, Router& b, uint64_t bandwidth = 0,
                       uint64_t delay = 0) {
  const Port back = OppositePort(port);
  a.ConnectPort(port, b.Position(), b.IsInternal(), &b.OpenPort(back));
  b.ConnectPort(back, a.Position(), a.IsInternal(), &a.OpenPort(port));
  a.SetLinkAttr(port, bandwidth, delay);
  b.SetLinkAttr(back, bandwidth, delay);
}

// 一个路由器与一个交换节点之间的线。
inline void AttachSwitchLink(Router& a, Port port, PcieSwitch& b,
                             uint32_t sw_port, uint64_t bandwidth = 0,
                             uint64_t delay = 0) {
  a.ConnectPort(port, b.Position(), /*peer_internal=*/false,
                &b.OpenPort(sw_port));
  b.ConnectPort(sw_port, a.Position(), /*peer_is_switch=*/false,
                &a.OpenPort(port), bandwidth, delay);
  a.SetLinkAttr(port, bandwidth, delay);
}

// 两个交换节点之间的线。
inline void AttachSwitchLink(PcieSwitch& a, uint32_t a_port, PcieSwitch& b,
                             uint32_t b_port, uint64_t bandwidth = 0,
                             uint64_t delay = 0) {
  a.ConnectPort(a_port, b.Position(), /*peer_is_switch=*/true,
                &b.OpenPort(b_port), bandwidth, delay);
  b.ConnectPort(b_port, a.Position(), /*peer_is_switch=*/true,
                &a.OpenPort(a_port), bandwidth, delay);
}

}
}

#endif
