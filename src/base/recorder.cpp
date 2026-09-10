#include "base/recorder.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/runtime.h"
#include "base/varint.h"

namespace latch {

namespace {

constexpr char kSegMagic[4]     = {'S', 'M', 'L', 'S'};
constexpr char kTrailerMagic[4] = {'S', 'M', 'L', 'T'};
constexpr char kMetaMagic[4]    = {'S', 'M', 'L', 'M'};
constexpr uint32_t kMetaVersion = 1;

}

Recorder::Recorder() : Recorder("Recorder") {}

Recorder::Recorder(const std::string& path_prefix, std::size_t threshold)
    : prefix(path_prefix), flush_threshold(threshold) {}

Recorder::~Recorder() {
  Finalize();
}

void Recorder::SetPathPrefix(const std::string& v) {
  std::lock_guard<std::mutex> lk(mu);
  LOGCHECK(fp == nullptr, "Recorder: the .trace file is already open");
  prefix = v;
}

void Recorder::StartNew(const std::string& v) {
  std::lock_guard<std::mutex> lk(mu);
  CloseTrace();
  prefix = v;
  seg_index.clear();
  str_ids.clear();
  sig_labels.clear();
  off = 0;
  finalized = false;
}

void Recorder::EnsureOpen() {
  if (fp != nullptr) return;
  const std::string path = prefix + ".trace";
  fp = std::fopen(path.c_str(), "wb");
  LOGCHECK(fp != nullptr, "Recorder: open .trace file failed");
  off = 0;
}

void Recorder::WriteRaw(const void* data, std::size_t len) {
  const auto wrote = std::fwrite(data, 1, len, fp);
  LOGCHECK(wrote == len, "Recorder: short write to .trace");
  off += len;
}

void Recorder::AppendSegment(TraceSlot& slot) {
  if (!slot.primed || slot.event_count == 0) return;

  std::lock_guard<std::mutex> lk(mu);
  LOGCHECK(!finalized, "Recorder: AppendSegment after Finalize");
  EnsureOpen();

  const uint64_t seg_off  = off;
  const uint32_t body_len = static_cast<uint32_t>(slot.buf.size());

  WriteRaw(kSegMagic, 4);
  WriteRaw(&slot.id, 8);
  WriteRaw(&body_len, 4);
  WriteRaw(&slot.event_count, 4);
  WriteRaw(&slot.t_first, 8);
  WriteRaw(&slot.v_first, 8);
  WriteRaw(&slot.t_last, 8);
  WriteRaw(&slot.v_min, 8);
  WriteRaw(&slot.v_max, 8);
  if (body_len > 0) WriteRaw(slot.buf.data(), body_len);

  seg_index[slot.id].push_back(seg_off);

  slot.buf.clear();
  slot.event_count = 0;
  slot.primed = false;
  slot.t_first = 0;
  slot.v_first = 0;
  slot.v_min = 0;
  slot.v_max = 0;
}

void Recorder::CloseTrace() {
  if (fp == nullptr) return;
  std::fclose(fp);
  fp = nullptr;
}

uint64_t Recorder::InternString(uint64_t sig_id, const std::string& value) {
  std::lock_guard<std::mutex> lk(mu);
  uint64_t id;
  auto it = str_ids.find(value);
  if (it != str_ids.end()) {
    id = it->second;
  } else {
    id = str_ids.size();
    str_ids.emplace(value, id);
  }
  sig_labels[sig_id][id] = value;
  return id;
}

namespace {

void AppendJsonEscaped(std::string& out, const std::string& s) {
  out.push_back('"');
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

}

void Recorder::WriteStrings() {
  if (sig_labels.empty()) return;
  std::string out = "{\"labels\":{";
  bool first_sig = true;
  for (auto const& sig : sig_labels) {
    if (!first_sig) out.push_back(',');
    first_sig = false;
    out += '"';
    out += std::to_string(sig.first);
    out += "\":{";
    bool first_v = true;
    for (auto const& kv : sig.second) {
      if (!first_v) out.push_back(',');
      first_v = false;
      out += '"';
      out += std::to_string(kv.first);
      out += "\":";
      AppendJsonEscaped(out, kv.second);
    }
    out += '}';
  }
  out += "}}";

  const std::string path = prefix + ".strings.json";
  std::FILE* sf = std::fopen(path.c_str(), "wb");
  LOGCHECK(sf != nullptr, "Recorder: open .strings.json failed");
  const auto wrote = std::fwrite(out.data(), 1, out.size(), sf);
  std::fclose(sf);
  LOGCHECK(wrote == out.size(), "Recorder: short write to .strings.json");
}

void Recorder::WriteMeta() {
  auto& mods = RT::GetModulePool().Modules();

  // 只写与这份波形有关的那些模块：记下过段的信号，沿 parent 链把它们的每一级
  // 祖先一并带上。模块表装的是这一轮建过的全部模块，阵列大了之后有几万个，没记
  // 波形的那些写进去只是让读波形的那一侧白拿一遍。名字是相对名、层次靠 parent，
  // 所以祖先必须补齐，缺一级读的那一侧就拼不出路径。
  std::unordered_set<uint64_t> keep;
  for (auto const& kv : seg_index) {
    uint64_t id = kv.first;
    // insert 返回 false 说明这条链上面那一段已经收过了。
    while (id != 0 && keep.insert(id).second) {
      auto it = mods.find(id);
      if (it == mods.end()) break;
      id = it->second.first;
    }
  }

  std::vector<const std::pair<const uint64_t,
                              std::pair<uint64_t, std::string>>*> picked;
  for (auto const& kv : mods) {
    if (keep.count(kv.first) != 0) picked.push_back(&kv);
  }
  const uint64_t record_cnt = picked.size();

  WriteRaw(kMetaMagic, 4);
  const uint32_t ver = kMetaVersion;
  WriteRaw(&ver, 4);
  WriteRaw(&record_cnt, 8);

  for (auto const* kv : picked) {
    const uint64_t id        = kv->first;
    const uint64_t parent_id = kv->second.first;
    const std::string& name  = kv->second.second;
    const uint16_t name_len  = static_cast<uint16_t>(name.size());
    WriteRaw(&id, 8);
    WriteRaw(&parent_id, 8);
    WriteRaw(&name_len, 2);
    if (name_len > 0) WriteRaw(name.data(), name_len);
  }
}

void Recorder::Finalize() {
  std::lock_guard<std::mutex> lk(mu);
  if (finalized) return;
  finalized = true;

  EnsureOpen();

  const uint64_t trailer_off = off;
  const uint32_t signal_cnt  = static_cast<uint32_t>(seg_index.size());

  WriteRaw(kTrailerMagic, 4);
  WriteRaw(&signal_cnt, 4);
  for (auto const& kv : seg_index) {
    const uint64_t sig_id    = kv.first;
    const uint32_t seg_count = static_cast<uint32_t>(kv.second.size());
    WriteRaw(&sig_id, 8);
    WriteRaw(&seg_count, 4);
    if (seg_count > 0) WriteRaw(kv.second.data(), seg_count * sizeof(uint64_t));
  }

  const uint64_t meta_off = off;
  WriteMeta();

  WriteRaw(&trailer_off, 8);
  WriteRaw(&meta_off, 8);

  CloseTrace();
  WriteStrings();
}

void Recorder::Reset() {
  std::lock_guard<std::mutex> lk(mu);
  CloseTrace();
  std::remove((prefix + ".trace").c_str());
  std::remove((prefix + ".strings.json").c_str());
  seg_index.clear();
  str_ids.clear();
  sig_labels.clear();
  off = 0;
  finalized = false;
}

}
