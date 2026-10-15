#ifndef _LATCH_TEST_BACHFLOW_INPUT_H_
#define _LATCH_TEST_BACHFLOW_INPUT_H_

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "bach/ip/bundle_load.h"
#include "test/bach/ip/chip/ep1_exact_common.h"

namespace latch {
namespace bach {
namespace ep1exact {

struct DteInputModeScope {
  uint64_t shift = DteSegValidShift();
  bool drops_tail = DteInboundDropsTail();
  ~DteInputModeScope() {
    DteSegValidShift() = shift;
    DteInboundDropsTail() = drops_tail;
  }
};

inline ByteBlock ReadInputBytes(std::string const &path, uint64_t expected) {
  std::ifstream in(path, std::ios::binary);
  LOGCHECK(in.good(), "转换后的权重文件打不开。");
  ByteBlock bytes(std::istreambuf_iterator<char>{in}, {});
  LOGCHECK(bytes.size() == expected, "转换后的权重文件长度不符。");
  return bytes;
}

inline LogicalMap LoadAdapterInput(std::vector<Chip *> const &chips,
                                   std::string const &root, std::string const &name,
                                   uint64_t *weight_count) {
  std::string directory = root + "/" + name;
  std::ifstream maps(directory + "/core-map.txt");
  LOGCHECK(maps.good(), "适配器缺 core-map.txt。");
  LogicalMap logical;
  uint64_t chip, physical;
  int64_t index;
  std::string role, line;
  while (std::getline(maps, line)) {
    if (line.empty() || line.front() == '#') continue;
    std::istringstream fields(line);
    std::string extra;
    LOGCHECK(bool(fields >> chip >> physical >> index >> role) && !(fields >> extra),
             "core-map 行不完整或字段过多。");
    LOGCHECK(chip < chips.size() && physical < chips[chip]->CoreNum() &&
                 index >= -1 && index <= 8,
             "core-map 越界。");
    LOGCHECK(logical.emplace(std::make_pair(chip, physical), index).second, "core-map 重复。");
  }
  LOGCHECK(maps.eof() && logical.size() == chips.size() * kChipCoreNum, "core-map 不完整。");
  // 在 LoadBundle 装载 HEX 镜像之前启用对应 RV 的 DTCM 别名。
  std::ifstream config(directory + "/" + name + ".bachir");
  LOGCHECK(config.good(), "适配器配置文件打不开。");
  while (std::getline(config, line)) {
    std::istringstream fields(line);
    std::string tag, image;
    uint64_t unit;
    if (!(fields >> tag) || tag != "KERNELCORE") continue;
    LOGCHECK(bool(fields >> chip >> physical >> unit >> image), "KERNELCORE 不完整。");
    LOGCHECK(chip < chips.size() && physical < chips[chip]->CoreNum() && unit < 3,
             "KERNELCORE 越界。");
    chips[chip]->GetCore(physical).Rv(unit).Exec().SetBachflowAbi();
  }
  auto stats = LoadBundle(chips, root, name);
  LOGCHECK(stats.chips == chips.size(), "适配器配置 chip 数不符。");
  // BACHIR11 的容器仍沿用。真实包不允许旧测试的正文丢尾行为。
  DteInboundDropsTail() = false;
  std::ifstream weights(directory + "/weights.txt");
  LOGCHECK(weights.good(), "适配器缺 weights.txt。");
  std::set<std::tuple<uint64_t, uint64_t, uint64_t>> seen;
  uint64_t base, bytes, scale_bytes;
  std::string data_file, scale_file;
  *weight_count = 0;
  while (std::getline(weights, line)) {
    if (line.empty() || line.front() == '#') continue;
    std::istringstream fields(line);
    std::string extra;
    LOGCHECK(bool(fields >> chip >> physical >> base >> bytes >> scale_bytes >> data_file >> scale_file) &&
                 !(fields >> extra),
             "权重行不完整或字段过多。");
    LOGCHECK(chip < chips.size() && physical < chips[chip]->CoreNum() &&
                 !chips[chip]->GetCore(physical).Bad() && bytes == 98304 &&
                 scale_bytes == bytes / 32 && base <= 32 * 1024 * 1024 - bytes &&
                 base % 32 == 0 && data_file.front() != '/' && scale_file.front() != '/' &&
                 data_file.find("..") == std::string::npos && scale_file.find("..") == std::string::npos,
             "转换后的权重记录越界或格式不符。");
    LOGCHECK(seen.emplace(chip, physical, base).second, "转换后的权重记录重复。");
    auto &mem = chips[chip]->GetCore(physical).Mmem();
    mem.Poke(base, ReadInputBytes(directory + "/" + data_file, bytes));
    mem.PokeScale(base, ReadInputBytes(directory + "/" + scale_file, scale_bytes));
    ++*weight_count;
  }
  LOGCHECK(weights.eof() && *weight_count != 0, "转换后的权重记录不完整。");
  return logical;
}

} // namespace ep1exact
} // namespace bach
} // namespace latch

#endif
