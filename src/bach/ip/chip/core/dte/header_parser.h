#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"
#include <deque>
#include <memory>
#include <string>
namespace latch {
namespace bach {
// Elastic RX bridge absorbs the registered ready delay. It retains each
// parsed beat until Lane acknowledges its sequence, including the first beat.
class HeaderParser : public BachModule {
public:
  HeaderParser(ClockPtr clock, const std::string &name, uint64_t parent = 0,
               bool tick = true)
      : BachModule(clock, name, parent, tick),
        from_router(std::make_shared<CoreDataPort>(clock)),
        payload(std::make_shared<PayloadPort>(clock)), parsed(clock),
        beats(clock) {}
  CoreDataPort &FromRouter() { return *from_router; }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    from_router = std::move(p);
  }
  std::shared_ptr<PayloadPort> PayloadPtr() const { return payload; }
  uint64_t Parsed() const { return parsed.Get(); }
  uint64_t Beats() const { return beats.Get(); }
  bool Quiescent() const override {
    return !in_frame && queue.empty() && !pending;
  }

protected:
  void Step() override {
    if (pending && payload->accepted_seq.Get() == pending)
      pending = 0;
    auto beat = ReadCoreData(*from_router);
    if (beat.valid && beat.seq != seen) {
      LOGCHECK(queue.size() < 4, "HeaderParser: RX bridge overflow");
      seen = beat.seq;
      queue.push_back(beat);
    }
    if (!pending) {
      payload->Idle();
      if (!queue.empty() && payload->Ready()) {
        auto next = queue.front();
        queue.pop_front();
        ++beat_count;
        uint64_t offset = count;
        if (!in_frame) {
          ++frames;
          ++parse_count;
          count = 0;
          offset = 0;
          expected = next.msg ? next.msg->TotalBytes() : 0;
          error = (!next.msg || expected > kMaxTaskBytes ||
                   next.msg->path_id > 255 || next.msg->user_id > 65535)
                      ? 8
                      : 0;
        }
        count += next.bytes;
        if (next.bytes > 256 || count > expected ||
            (next.last && count != expected))
          error = 8;
        auto message = next.msg ? next.msg : std::make_shared<Message>();
        payload->Drive(frames, next.bytes, offset, next.last, message, error);
        pending =
            ++payload_sequence; // Drive increments the producer-local sequence.
        in_frame = !next.last;
        if (next.last)
          count = 0;
      }
    }
    from_router->DriveReady(queue.size() < 2);
    parsed = parse_count;
    beats = beat_count;
    TracePerCycle("parsed", parse_count);
  }

private:
  std::shared_ptr<CoreDataPort> from_router;
  std::shared_ptr<PayloadPort> payload;
  std::deque<CoreDataView> queue;
  bool in_frame = false;
  uint64_t seen = 0, pending = 0, payload_sequence = 0, frames = 0, count = 0,
           expected = 0, error = 0, parse_count = 0, beat_count = 0;
  Logic64 parsed, beats;
};
} // namespace bach
} // namespace latch
#endif
