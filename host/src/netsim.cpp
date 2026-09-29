#include "netsim.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "util.h"

namespace aoi {

std::string NetSimConfig::describe() const {
  char b[160];
  snprintf(b, sizeof b, "rate=%.0fkbps loss=%.1f%% burst=%.1f delay=%dms jitter=%dms queue=%dms", kbps, loss, burst,
           delay_ms, jitter_ms, queue_ms);
  return b;
}

bool parse_netsim(const std::string &s, NetSimConfig &c) {
  std::stringstream ss(s);
  std::string kv;
  while (std::getline(ss, kv, ',')) {
    auto eq = kv.find('=');
    if (eq == std::string::npos) return false;
    std::string k = kv.substr(0, eq);
    double v = atof(kv.c_str() + eq + 1);
    if (k == "rate") c.kbps = v;
    else if (k == "loss") c.loss = v;
    else if (k == "burst") c.burst = std::max(1.0, v);
    else if (k == "delay") c.delay_ms = int(v);
    else if (k == "jitter") c.jitter_ms = int(v);
    else if (k == "queue") c.queue_ms = int(v);
    else return false;
  }
  return true;
}

NetSim::NetSim(NetSimConfig cfg, Deliver d) : cfg_(cfg), deliver_(std::move(d)) {
  rng_ = (uint64_t(random_u32()) << 32) | random_u32() | 1;
  timeBeginPeriod(1);
  thread_ = std::thread([this] { run(); });
}

NetSim::~NetSim() {
  { std::lock_guard lk(mu_); stop_ = true; }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  timeEndPeriod(1);
}

double NetSim::rnd() {  // xorshift64*
  rng_ ^= rng_ >> 12; rng_ ^= rng_ << 25; rng_ ^= rng_ >> 27;
  return double((rng_ * 2685821657736338717ull) >> 11) / double(1ull << 53);
}

void NetSim::send(std::vector<uint8_t> &&pkt) {
  std::lock_guard lk(mu_);
  int64_t now = now_us();
  // Gilbert-Elliott: P(good->bad) and P(bad->good) chosen so the long-run loss
  // is `loss` and a loss run averages `burst` packets.
  double L = cfg_.loss / 100.0;
  if (L > 0) {
    double p_bg = 1.0 / cfg_.burst;
    double p_gb = std::min(1.0, L * p_bg / std::max(1e-9, 1.0 - L));
    bad_ = bad_ ? rnd() >= p_bg : rnd() < p_gb;
    if (bad_) { ++drop_l_; return; }
  }
  int64_t depart = now;
  if (cfg_.kbps > 0) {
    int64_t tx = int64_t(double(pkt.size() + 28) * 8.0 * 1000.0 / cfg_.kbps);  // + IP/UDP
    if (link_free_ - now > int64_t(cfg_.queue_ms) * 1000) { ++drop_q_; return; }  // tail drop
    depart = std::max(now, link_free_) + tx;
    link_free_ = depart;
  }
  int64_t at = depart + int64_t(cfg_.delay_ms) * 1000 + int64_t(rnd() * cfg_.jitter_ms * 1000);
  at = std::max(at, last_deliver_);  // jitter without reordering, like most real paths
  last_deliver_ = at;
  q_.push({at, order_++, std::move(pkt)});
  cv_.notify_all();
}

void NetSim::run() {
  std::unique_lock lk(mu_);
  while (!stop_) {
    if (q_.empty()) { cv_.wait(lk); continue; }
    int64_t wait = q_.top().at - now_us();
    if (wait > 0) {
      cv_.wait_for(lk, std::chrono::microseconds(wait));
      continue;
    }
    std::vector<uint8_t> pkt = std::move(const_cast<Item &>(q_.top()).pkt);
    q_.pop();
    lk.unlock();
    deliver_(std::move(pkt));
    lk.lock();
  }
}

}  // namespace aoi
