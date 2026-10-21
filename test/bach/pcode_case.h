#ifndef LATCH_TEST_PCODE_CASE_H
#define LATCH_TEST_PCODE_CASE_H
#include <array>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>
#include "bach/common/message.h"
namespace pcode_test {
using namespace latch::bach;
struct Fixture {
  uint32_t user = 0, token = 1, mask = 63;
  std::array<std::vector<std::pair<uint32_t,float>>,6> experts;
  std::vector<uint8_t> input, scales;
  struct Weight {
    uint32_t chip, core, stage, expert, rows;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> values, scales;
  };
  std::vector<Weight> weights;
  static uint32_t U32(std::ifstream& in) {
    uint8_t b[4]; in.read(reinterpret_cast<char*>(b),4);
    LOGCHECK(in.good(), "truncated pcode fixture");
    return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24;
  }
  static std::vector<uint8_t> Bytes(std::ifstream& in, uint32_t n) {
    std::vector<uint8_t> v(n); in.read(reinterpret_cast<char*>(v.data()), n);
    LOGCHECK(in.good(), "truncated pcode fixture bytes"); return v;
  }
  explicit Fixture(char const* path) {
    std::ifstream in(path, std::ios::binary);
    LOGCHECK(in.good(), "cannot open pcode fixture");
    auto magic=Bytes(in,8);
    LOGCHECK(std::string(magic.begin(),magic.end())=="PCDTST02", "bad fixture version");
    user=U32(in);token=U32(in);mask=U32(in);
    for(auto& group:experts) {
      uint32_t count=U32(in);LOGCHECK(count<=8,"too many experts");
      for(uint32_t i=0;i<count;++i) {uint32_t id=U32(in);float w=numeric::FloatOf(U32(in));group.push_back({id,w});}
    }
    input=Bytes(in,6144);scales=Bytes(in,192);
    uint32_t n=U32(in);
    for(uint32_t i=0;i<n;++i) {
      Weight w;w.chip=U32(in);w.core=U32(in);w.stage=U32(in);w.expert=U32(in);w.rows=U32(in);
      uint32_t taps=U32(in);LOGCHECK(taps>0 && taps<=64,"invalid taps");
      for(uint32_t t=0;t<taps;++t) w.indices.push_back(U32(in));
      w.values=Bytes(in,w.rows*taps);w.scales=Bytes(in,w.rows*taps);weights.push_back(std::move(w));
    }
    LOGCHECK(in.peek()==EOF,"extra fixture data");
  }
  MessagePtr Message() const {
    auto m=std::make_shared<latch::bach::Message>();m->path_id=63;m->path_core_mask=mask;m->user_id=user;m->token_id=token;
    constexpr uint32_t topk_stride=56, input_offset=6*topk_stride;
    m->size=input_offset+6144+192;m->payload.assign(m->size,0);
    for(uint32_t ep=0;ep<6;++ep) {
      m->payload[ep*topk_stride]=experts[ep].size();
      for(uint32_t j=0;j<experts[ep].size();++j) {
        auto e=experts[ep][j];uint32_t at=ep*topk_stride+2+j*6, word=numeric::BitsOf(e.second);
        m->payload[at]=e.first;m->payload[at+1]=e.first>>8;
        for(uint32_t k=0;k<4;++k)m->payload[at+2+k]=word>>(8*k);
      }
    }
    std::copy(input.begin(),input.end(),m->payload.begin()+input_offset);
    std::copy(scales.begin(),scales.end(),m->payload.begin()+input_offset+6144);return m;
  }
  void Load(std::vector<Chip*> const& chips, std::string const& root) const {
    std::ifstream ir(root+"/pcode_ep6_tp8/pcode_ep6_tp8.bachir");
    std::map<std::tuple<uint32_t,uint32_t,uint32_t>,std::array<uint32_t,10>> templates;
    std::string line;
    while(std::getline(ir,line)) {
      std::istringstream s(line);std::string tag,offset,value;uint32_t chip,core,unit;
      if(!(s>>tag) || tag!="DSAREG")continue;s>>chip>>core>>unit>>offset>>value;
      uint32_t off=std::stoul(offset,nullptr,0);if(unit!=1 || off<0x1000 || off>=0x1100)continue;
      uint32_t f=(off-0x1000)%128/4;if(f<10)templates[{chip,core,(off-0x1000)/128}][f]=std::stoul(value,nullptr,0);
    }
    for(auto const& w:weights) {
      auto const& tpl=templates.at({w.chip,w.core,w.stage});uint32_t kb=tpl[0]>>16;
      LOGCHECK(w.rows==(tpl[0]&65535)*128,"fixture shape mismatch");
      auto& mem=chips.at(w.chip)->GetCore(w.core).Mmem();
      for(uint32_t n=0;n<w.rows/128;++n) {
        std::map<uint32_t,ByteBlock> tiles,sf;
        for(uint32_t t=0;t<w.indices.size();++t) {
          uint32_t ki=w.indices[t],k=ki/64;LOGCHECK(k<kb,"K index exceeds matrix");
          if(!tiles.count(k)){tiles[k]=ByteBlock(8192,0);sf[k]=ByteBlock(256,127);}
          for(uint32_t col=0;col<128;++col) {
            uint32_t ix=(n*128+col)*w.indices.size()+t;
            tiles[k][col*64+ki%64]=w.values[ix];sf[k][col*2+(ki%64)/32]=w.scales[ix];
          }
        }
        for(auto const& tile:tiles) {
          uint64_t at=uint64_t(tpl[6]-0x48c00000)+uint64_t(w.expert)*tpl[7]+(n*kb+tile.first)*8192;
          mem.Poke(at,tile.second);mem.PokeScale(at,sf.at(tile.first));
        }
      }
    }
  }
};
}
#endif
