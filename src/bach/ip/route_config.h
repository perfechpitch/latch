#ifndef _LATCH_BACH_IP_ROUTE_CONFIG_
#define _LATCH_BACH_IP_ROUTE_CONFIG_

// 路由要用的全局表。装配期由 Map 的编译产物算出来，运行期所有 Router 共读一份。
//
// 每个 Router 只知道自己的坐标与十二个端口接了谁，这不足以定路：有两类目的地要先
// 换算成一个阵列内的坐标，才轮得到方向判断。
//
//   外部节点  它的坐标不在核阵列内，先换成接它的那个网关核的坐标；到了网关核，
//             再从登记的那个端口把包送出去
//   跨 chip   目的地在别的 chip 上时，先换成本 chip 朝那个方向的网关核坐标；
//             没登记网关的方向按原目标直接走，靠端口是否存在自然收敛
//
// 换算只改"下一步往哪去"，不改包上的最终目的坐标：最终目的一路带到终点，中途每个
// Router 各自重算一次下一跳。

#include <cstdint>
#include <unordered_map>

#include "base/log.h"
#include "bach/common/packet.h"
#include "bach/tables/loader.h"

namespace latch {
namespace bach {

// 一个外部节点接在哪里、从哪个端口出去。
struct ExtRoute {
  Coord gateway;
  Port eject = Port::kUnknown;
};

struct RouteConfig {
  Dim dim;
  std::unordered_map<uint64_t, ExtRoute> ext_nodes;
  std::unordered_map<uint64_t, Coord> chip_gateways;

  static uint64_t GatewayKey(uint32_t chip_id, Direction dir) {
    return (static_cast<uint64_t>(chip_id) << 3) | static_cast<uint32_t>(dir);
  }

  void AddExtNode(Coord ext, Coord gateway, Port eject) {
    LOGCHECK(eject != Port::kUnknown, "RouteConfig: eject port is unknown.");
    ext_nodes[EncodeCoord(ext)] = ExtRoute{gateway, eject};
  }

  void AddChipGateway(uint32_t chip_id, Direction dir, Coord gateway) {
    chip_gateways[GatewayKey(chip_id, dir)] = gateway;
  }

  ExtRoute const* ExtAt(Coord c) const {
    auto it = ext_nodes.find(EncodeCoord(c));
    return it == ext_nodes.end() ? nullptr : &it->second;
  }

  Coord const* GatewayOf(uint32_t chip_id, Direction dir) const {
    auto it = chip_gateways.find(GatewayKey(chip_id, dir));
    return it == chip_gateways.end() ? nullptr : &it->second;
  }

  // 从本 chip 看目标 chip 在哪个方向。行差优先于列差，与网关规则的登记方式一致。
  static Direction DirectionTo(Coord src_chip, Coord dst_chip) {
    if (src_chip.row < dst_chip.row) return Direction::kBottom;
    if (src_chip.row > dst_chip.row) return Direction::kTop;
    if (src_chip.col < dst_chip.col) return Direction::kRight;
    return Direction::kLeft;
  }
};

}
}

#endif
