#ifndef _LATCH_RECORDER_
#define _LATCH_RECORDER_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace latch {

struct TraceSlot {
  uint64_t id = 0;
  uint64_t t_first = 0;
  uint64_t v_first = 0;
  uint64_t t_last = 0;
  uint64_t v_last = 0;
  uint64_t v_min = 0;
  uint64_t v_max = 0;
  uint32_t event_count = 0;
  bool     primed = false;
  std::vector<uint8_t> buf;
};

class Recorder {
 public:
  static constexpr std::size_t kDefaultFlushThreshold = 4 * 1024 * 1024;

  Recorder();
  explicit Recorder(const std::string& path_prefix,
                    std::size_t flush_threshold = kDefaultFlushThreshold);
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  ~Recorder();

  void AppendSegment(TraceSlot& slot);
  void Finalize();
  void Reset();

  uint64_t InternString(uint64_t sig_id, const std::string& value);

  std::size_t FlushThreshold() const { return flush_threshold; }
  void SetFlushThreshold(std::size_t v) { flush_threshold = v; }
  const std::string& PathPrefix() const { return prefix; }

 private:
  void EnsureOpen();
  void WriteRaw(const void* data, std::size_t len);
  void WriteMeta();
  void WriteStrings();
  void CloseTrace();

  std::string prefix;
  std::size_t flush_threshold;
  std::FILE* fp = nullptr;
  uint64_t off = 0;
  std::unordered_map<uint64_t, std::vector<uint64_t>> seg_index;
  std::unordered_map<std::string, uint64_t> str_ids;
  std::unordered_map<uint64_t, std::map<uint64_t, std::string>> sig_labels;
  bool finalized = false;
  std::mutex mu;
};

}

#endif
