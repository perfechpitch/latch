#ifndef _LATCH_UTILS_TRACE_READER_
#define _LATCH_UTILS_TRACE_READER_

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/varint.h"

namespace latch {

struct TraceMetaRecord {
  uint64_t id = 0;
  uint64_t parent_id = 0;
  std::string name;
};

struct TraceSegmentHeader {
  uint64_t signal_id = 0;
  uint32_t body_len = 0;
  uint32_t event_count = 0;
  uint64_t t_first = 0;
  uint64_t v_first = 0;
  uint64_t t_last = 0;
  uint64_t v_min = 0;
  uint64_t v_max = 0;
  uint64_t body_off = 0;
};

struct TraceOffsets {
  uint64_t trailer_off = 0;
  uint64_t meta_off = 0;
};

inline std::vector<uint8_t> TraceSlurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

template <typename T>
inline T TraceReadAt(const std::vector<uint8_t>& buf, std::size_t off) {
  T v;
  std::memcpy(&v, buf.data() + off, sizeof(T));
  return v;
}

inline TraceOffsets TraceReadFooter(const std::vector<uint8_t>& buf) {
  TraceOffsets o;
  if (buf.size() < 16) return o;
  o.trailer_off = TraceReadAt<uint64_t>(buf, buf.size() - 16);
  o.meta_off = TraceReadAt<uint64_t>(buf, buf.size() - 8);
  return o;
}

inline std::vector<TraceMetaRecord> TraceParseMeta(
    const std::vector<uint8_t>& buf) {
  TraceOffsets o = TraceReadFooter(buf);
  uint64_t cnt = TraceReadAt<uint64_t>(buf, o.meta_off + 8);
  std::vector<TraceMetaRecord> out;
  out.reserve(cnt);
  std::size_t off = o.meta_off + 16;
  for (uint64_t i = 0; i < cnt; ++i) {
    TraceMetaRecord r;
    r.id = TraceReadAt<uint64_t>(buf, off + 0);
    r.parent_id = TraceReadAt<uint64_t>(buf, off + 8);
    uint16_t nlen = TraceReadAt<uint16_t>(buf, off + 16);
    r.name.assign(reinterpret_cast<const char*>(buf.data() + off + 18), nlen);
    out.push_back(std::move(r));
    off += 18 + nlen;
  }
  return out;
}

inline std::map<uint64_t, std::vector<TraceSegmentHeader>> TraceParseDataIndex(
    const std::vector<uint8_t>& buf) {
  TraceOffsets o = TraceReadFooter(buf);
  uint32_t signal_cnt = TraceReadAt<uint32_t>(buf, o.trailer_off + 4);
  std::map<uint64_t, std::vector<TraceSegmentHeader>> by_sig;
  std::size_t cur = o.trailer_off + 8;
  for (uint32_t i = 0; i < signal_cnt; ++i) {
    uint64_t sig_id = TraceReadAt<uint64_t>(buf, cur);
    uint32_t seg_count = TraceReadAt<uint32_t>(buf, cur + 8);
    cur += 12;
    auto& vec = by_sig[sig_id];
    for (uint32_t s = 0; s < seg_count; ++s) {
      uint64_t seg_off = TraceReadAt<uint64_t>(buf, cur);
      cur += 8;
      TraceSegmentHeader h;
      h.signal_id = TraceReadAt<uint64_t>(buf, seg_off + 4);
      h.body_len = TraceReadAt<uint32_t>(buf, seg_off + 12);
      h.event_count = TraceReadAt<uint32_t>(buf, seg_off + 16);
      h.t_first = TraceReadAt<uint64_t>(buf, seg_off + 20);
      h.v_first = TraceReadAt<uint64_t>(buf, seg_off + 28);
      h.t_last = TraceReadAt<uint64_t>(buf, seg_off + 36);
      h.v_min = TraceReadAt<uint64_t>(buf, seg_off + 44);
      h.v_max = TraceReadAt<uint64_t>(buf, seg_off + 52);
      h.body_off = seg_off + 60;
      vec.push_back(h);
    }
  }
  return by_sig;
}

inline std::vector<std::pair<uint64_t, uint64_t>> TraceDecodeSegment(
    const std::vector<uint8_t>& buf, const TraceSegmentHeader& h) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  out.reserve(h.event_count);
  if (h.event_count == 0) return out;
  out.emplace_back(h.t_first, h.v_first);
  uint64_t t = h.t_first;
  uint64_t v = h.v_first;
  std::size_t pos = h.body_off;
  const std::size_t end = h.body_off + h.body_len;
  for (uint32_t i = 1; i < h.event_count; ++i) {
    std::size_t used = 0;
    uint64_t dt = VarintDecode(buf.data() + pos, end - pos, used);
    pos += used;
    uint64_t dvz = VarintDecode(buf.data() + pos, end - pos, used);
    pos += used;
    t += dt;
    int64_t dv = ZigZagDecode(dvz);
    v = static_cast<uint64_t>(static_cast<int64_t>(v) + dv);
    out.emplace_back(t, v);
  }
  return out;
}

}

#endif
