#ifndef LATCH_TEST_PCODE_DENSE_MODEL_H
#define LATCH_TEST_PCODE_DENSE_MODEL_H
#include <fcntl.h>
#include <cmath>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pcode_test {
inline void ValidateDenseFixture(Fixture const& fixture) {
  uint64_t count = 0, routed = 0;
  double sum = 0;
  std::set<uint64_t> chosen;
  uint64_t offsets[] = {0,43,86,129,172,215,256};
  for (uint64_t ep = 0; ep < 6; ++ep) {
    if (!fixture.experts[ep].empty()) routed |= 1ull << ep;
    for (auto const& expert : fixture.experts[ep]) {
      LOGCHECK(expert.first < offsets[ep+1]-offsets[ep] &&
               chosen.insert(offsets[ep]+expert.first).second,
               "GLM5 requires eight distinct valid routed experts");
      LOGCHECK(std::isfinite(expert.second) && expert.second >= 0,
               "invalid GLM5 TopK coefficient");
      sum += expert.second; ++count;
    }
  }
  uint64_t shared = fixture.user % 6;
  LOGCHECK(count == 8 && std::abs(sum-1) < 1e-6 && fixture.user <= 1023,
           "invalid GLM5 single-token identity/topK");
  LOGCHECK(fixture.mask == (routed | (1ull << shared)), "GLM5 EP mask mismatch");
  std::cerr << "pcode mask routed " << routed << ", shared EP " << shared
            << ", actual " << fixture.mask << "\n";
}
struct ModelMapping {
  uint8_t const* data = nullptr;
  size_t bytes = 0;
  explicit ModelMapping(std::string const& path) {
    int fd = open(path.c_str(), O_RDONLY);
    LOGCHECK(fd >= 0, "cannot open complete dense model image");
    struct stat st;
    LOGCHECK(fstat(fd, &st) == 0 && st.st_size > 0, "invalid dense image");
    bytes = st.st_size;
    void* mapped = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    LOGCHECK(mapped != MAP_FAILED, "cannot map dense image");
    data = static_cast<uint8_t const*>(mapped);
  }
  ~ModelMapping() { if (data) munmap(const_cast<uint8_t*>(data), bytes); }
};

inline void LoadDenseModel(std::vector<latch::bach::Chip*> const& chips,
                           std::string const& root, std::string const& bundle) {
  std::ifstream ir(bundle + "/pcode_ep6_tp8/pcode_ep6_tp8.bachir");
  std::map<std::tuple<uint32_t,uint32_t,uint32_t>,std::array<uint32_t,10>> templates;
  std::string line;
  while (std::getline(ir, line)) {
    std::istringstream s(line); std::string tag, off, value;
    uint32_t chip, core, unit;
    if (!(s >> tag) || tag != "DSAREG") continue;
    s >> chip >> core >> unit >> off >> value;
    uint32_t address = std::stoul(off,nullptr,0);
    if (unit != 1 || address < 0x1000 || address >= 0x1100) continue;
    uint32_t field = (address - 0x1000) % 128 / 4;
    if (field < 10) templates[{chip,core,(address - 0x1000)/128}][field] = std::stoul(value,nullptr,0);
  }
  std::ifstream index(root + "/layout.tsv");
  LOGCHECK(index.good(), "missing dense model index");
  uint64_t chip, core, stage, base, count, stride, bytes, loaded = 0, total = 0;
  std::string filename;
  std::set<std::tuple<uint64_t,uint64_t,uint64_t>> seen;
  while (index >> chip >> core >> stage >> base >> count >> stride >> bytes >> filename) {
    LOGCHECK(chip < 48 && core < 10 && stage < 2 && filename.find('/') == std::string::npos,
             "invalid dense image index");
    LOGCHECK(seen.insert({chip,core,stage}).second, "duplicate dense image");
    auto const& t = templates.at({chip,core,stage});
    LOGCHECK(base == uint64_t(t[6]) - 0x48c00000 && stride == t[7], "dense image address mismatch");
    LOGCHECK(stride == (stage == 0 ? 393216u : 196608u), "unexpected GLM5 shape");
    LOGCHECK((t[0]&65535) == (stage == 0 ? 4u : 6u) &&
             (t[0]>>16) == (stage == 0 ? 12u : 4u), "unexpected GLM5 primitive dimensions");
    LOGCHECK(count == (chip / 8 == 5 ? 42u : 44u) && bytes == count * stride,
             "must preload every routed expert and shared replica");
    auto image = std::make_shared<ModelMapping>(root + "/" + filename);
    LOGCHECK(image->bytes == bytes + bytes/32, "dense image size mismatch");
    auto& mem = chips.at(chip)->GetCore(core).Mmem();
    mem.PreloadImage(base,bytes,image->data,image->data + bytes,image);
    LOGCHECK(mem.Peek(base,128) == latch::bach::ByteBlock(image->data,image->data+128),
             "dense image readback mismatch");
    LOGCHECK(mem.PeekScale(base + bytes - 128,4) == latch::bach::ByteBlock(
                 image->data + bytes + bytes/32 - 4, image->data + bytes + bytes/32),
             "dense image scale readback mismatch");
    total += bytes; ++loaded;
  }
  LOGCHECK(index.eof() && loaded == 768 && seen.size() == templates.size(), "incomplete dense model");
  std::cerr << "pcode dense model images " << loaded << ", data bytes " << total
            << ", routed experts 256, shared replicas 6\n";
}
}
#endif
