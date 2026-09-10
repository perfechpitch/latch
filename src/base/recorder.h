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
  // 落盘路径。全局那个 Recorder 是默认构造的，前缀恒为 Recorder，写到进程的当前
  // 目录；一个进程里跑多轮就会一轮盖一轮。要分开存就在开写之前改掉前缀。
  // 第一段落盘之后文件已经打开，这时再改前缀，改到的与写进去的对不上，所以拒绝。
  void SetPathPrefix(const std::string& v);
  // 换个前缀重新开一份。上一份已经写好的文件留着不动，这一份从零攒。一个进程
  // 里跑几轮、每轮要各自一份波形时用它：Finalize 之后只是把文件收了尾，状态还
  // 停在「已收尾」上，不清掉就再也写不出第二份的索引。
  void StartNew(const std::string& v);

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
