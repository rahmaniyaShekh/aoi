// A bad network in a box, for testing against a real browser.
//
//   --simulate "rate=48,loss=5,burst=3,delay=120,jitter=40,queue=300"
//
// Outgoing RTP passes through a bottleneck link: a serialisation rate
// (kbps) with a drop-tail queue (ms of backlog), then one-way delay with
// jitter, and Gilbert-Elliott loss (average `loss` %, mean burst `burst`
// packets). Chrome's DevTools throttling does not touch WebRTC, so this is
// how the congestion controller, RED, NACK and the listener's buffer are
// exercised end to end on one machine.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace aoi {

struct NetSimConfig {
  double kbps = 0;      // 0 = unlimited
  double loss = 0;      // percent
  double burst = 1;     // mean loss burst length, packets
  int delay_ms = 0, jitter_ms = 0, queue_ms = 300;
  bool active() const { return kbps > 0 || loss > 0 || delay_ms > 0 || jitter_ms > 0; }
  std::string describe() const;
};
bool parse_netsim(const std::string &s, NetSimConfig &out);

class NetSim {
 public:
  using Deliver = std::function<void(std::vector<uint8_t> &&)>;
  NetSim(NetSimConfig cfg, Deliver d);
  ~NetSim();
  void send(std::vector<uint8_t> &&pkt);
  uint64_t dropped_queue() const { return drop_q_; }
  uint64_t dropped_loss() const { return drop_l_; }

 private:
  void run();
  struct Item { int64_t at; uint64_t order; std::vector<uint8_t> pkt; };
  struct Later { bool operator()(const Item &a, const Item &b) const { return a.at != b.at ? a.at > b.at : a.order > b.order; } };
  NetSimConfig cfg_;
  Deliver deliver_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<Item, std::vector<Item>, Later> q_;
  std::thread thread_;
  bool stop_ = false, bad_ = false;
  int64_t link_free_ = 0, last_deliver_ = 0;
  uint64_t order_ = 0, drop_q_ = 0, drop_l_ = 0;
  uint64_t rng_;
  double rnd();
};

}  // namespace aoi
