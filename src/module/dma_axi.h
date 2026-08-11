#ifndef _DMA_AXI_
#define _DMA_AXI_

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/module.h"
#include "module/axi.h"

namespace latch {

class DmaAxi : public ClkModule {
 public:
  struct DmaCmd {
    uint64_t src;
    uint64_t dst;
    uint64_t len;
  };

  DmaAxi(ClockPtr clock, const AxiConfig& cfg = AxiConfig{},
         uint32_t cmdQueueSize = 8, const std::string& name = "dma")
      : ClkModule(clock),
        cfg_(cfg),
        cmdQueSize_(cmdQueueSize),
        name_(name) {}

  void BindCmdPort(std::shared_ptr<AxiMaster> busMaster) {
    cmdSlave_ = std::make_shared<AxiSlave>(clk, name_ + "_cmd", cfg_);
    busMaster->Bind(cmdSlave_);
  }
  void BindDataPort(std::shared_ptr<AxiSlave> busSlave) {
    dataMaster_ = std::make_shared<AxiMaster>(clk, name_ + "_data", cfg_);
    dataMaster_->Bind(busSlave);
  }

  static uint64_t CmdBeats(uint64_t data_bytes) {
    return (sizeof(DmaCmd) + data_bytes - 1) / data_bytes;
  }
  static std::vector<std::shared_ptr<std::vector<uint8_t>>> EncodeCmd(
      const DmaCmd& c, uint64_t data_bytes) {
    uint64_t nb = CmdBeats(data_bytes);
    std::vector<uint8_t> raw(nb * data_bytes, 0);
    std::memcpy(raw.data(), &c, sizeof(DmaCmd));
    std::vector<std::shared_ptr<std::vector<uint8_t>>> beats(nb);
    for (uint64_t i = 0; i < nb; ++i)
      beats[i] = std::make_shared<std::vector<uint8_t>>(
          raw.begin() + i * data_bytes, raw.begin() + (i + 1) * data_bytes);
    return beats;
  }

  void Cycle() override {
    DelayCycle(1);
    IntakeCmd();
    DriveCopy();
  }

 private:
  enum class Phase {
    kReadIssue,
    kReadCollect,
    kWriteIssue,
    kWritePush,
    kWriteWaitB,
    kSendCmdB,
  };
  struct Job {
    DmaCmd cmd;
    uint64_t awId;
    bool started = false;
    Phase phase = Phase::kReadIssue;
    uint64_t id = 0;
    uint64_t nbeats = 0;
    uint8_t size = 0;
    uint64_t lastBytes = 0;
    uint64_t wpushed = 0;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> buf;
  };
  struct CmdRecv {
    bool active = false;
    uint64_t awId = 0;
    std::vector<uint8_t> buf;
  };

  static uint8_t Log2(uint64_t v) {
    uint8_t s = 0;
    while ((uint64_t(1) << s) < v) ++s;
    return s;
  }

  void IntakeCmd() {
    if (!cmdSlave_ || !cmdSlave_->IsBound()) return;
    if (!recv_.active && jobs_.size() < cmdQueSize_) {
      if (auto aw = cmdSlave_->TryPopAW()) {
        recv_.active = true;
        recv_.awId = aw->id;
        recv_.buf.clear();
      }
    }
    if (recv_.active) {
      if (auto wb = cmdSlave_->TryPopW()) {
        recv_.buf.insert(recv_.buf.end(), wb->data->begin(), wb->data->end());
        if (wb->last) {
          Job j;
          std::memcpy(&j.cmd, recv_.buf.data(), sizeof(DmaCmd));
          j.awId = recv_.awId;
          jobs_.push_back(std::move(j));
          recv_ = CmdRecv{};
        }
      }
    }
  }

  void DriveCopy() {
    if (!dataMaster_ || !dataMaster_->IsBound()) return;
    if (jobs_.empty()) return;
    Job& job = jobs_.front();

    if (!job.started) {
      job.started = true;
      job.id = ++idCtr_;
      job.size = Log2(cfg_.data_bytes);
      if (job.cmd.len == 0) {
        job.nbeats = 0;
        job.phase = Phase::kSendCmdB;
      } else {
        job.nbeats = (job.cmd.len + cfg_.data_bytes - 1) / cfg_.data_bytes;
        job.lastBytes = job.cmd.len - (job.nbeats - 1) * cfg_.data_bytes;
        job.buf.reserve(job.nbeats);
        job.phase = Phase::kReadIssue;
      }
    }

    switch (job.phase) {
      case Phase::kReadIssue:
        if (dataMaster_->TryIssueRead(job.id, job.cmd.src,
                                      static_cast<uint8_t>(job.nbeats - 1),
                                      job.size))
          job.phase = Phase::kReadCollect;
        break;
      case Phase::kReadCollect:
        if (auto r = dataMaster_->TryPopR()) {
          job.buf.push_back(r->data);
          if (r->last) job.phase = Phase::kWriteIssue;
        }
        break;
      case Phase::kWriteIssue:
        if (dataMaster_->TryIssueWriteAddr(
                job.id, job.cmd.dst, static_cast<uint8_t>(job.nbeats - 1),
                job.size)) {
          job.wpushed = 0;
          job.phase = Phase::kWritePush;
        }
        break;
      case Phase::kWritePush:
        if (!dataMaster_->W().IsFull()) {
          bool last = (job.wpushed == job.nbeats - 1);
          std::shared_ptr<std::vector<uint8_t>> strb;
          if (last && job.lastBytes < cfg_.data_bytes) {
            strb = std::make_shared<std::vector<uint8_t>>(cfg_.data_bytes, 0);
            for (uint64_t i = 0; i < job.lastBytes; ++i) (*strb)[i] = 1;
          }
          dataMaster_->TryPushWBeat(job.buf[job.wpushed], last, strb);
          ++job.wpushed;
          if (last) job.phase = Phase::kWriteWaitB;
        }
        break;
      case Phase::kWriteWaitB:
        if (dataMaster_->TryPopB()) job.phase = Phase::kSendCmdB;
        break;
      case Phase::kSendCmdB:
        if (cmdSlave_ && !cmdSlave_->B().IsFull()) {
          cmdSlave_->TryPushB(job.awId);
          jobs_.pop_front();
        }
        break;
    }
  }

  AxiConfig cfg_;
  uint32_t cmdQueSize_;
  std::string name_;
  std::shared_ptr<AxiSlave> cmdSlave_;
  std::shared_ptr<AxiMaster> dataMaster_;
  std::deque<Job> jobs_;
  CmdRecv recv_;
  uint64_t idCtr_ = 0;
};

}
#endif
