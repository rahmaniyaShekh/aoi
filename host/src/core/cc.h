// Congestion control and the encoding plan, per listener.
//
// ASOW judged a link by its own send-queue backlog, which is the right signal
// on a LAN over TCP. Over the internet on UDP there is no queue of ours to
// watch -- the queue that matters is in somebody's router, and the first sign
// of it is packets arriving progressively later than they were sent. So:
//
//  * Delay-based: transport-wide feedback gives per-packet arrival times. A
//    trendline filter over (arrival - send) finds a growing queue within a
//    few hundred milliseconds, before a single packet is lost.
//  * Loss-based: bounds the rate when loss is heavy, holds it when moderate.
//  * Fallback: without transport feedback, RTT inflation above the path's
//    minimum does the delay detector's job from receiver reports.
//
// The resulting bitrate is then spent, not just on Opus, but on the right mix
// of Opus bitrate, redundancy (RED) and frame duration for the loss seen:
// on a lossy link, a lower bitrate that arrives intact beats a higher one
// that does not.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "rtp.h"
#include "util.h"

namespace aoi {

struct Plan {
  int target_bps = 0;   // what the controller allows on the wire
  int opus_bps = 0;     // encoder bitrate (per copy)
  int frame_ms = 20;    // 10/20/40/60
  int red = 0;          // redundant copies per packet (0-3)
  bool stereo = true;
  bool fec = false;     // Opus in-band FEC (LBRR), effective in SILK/hybrid modes
  int loss_pct = 0;     // hint to the encoder
  std::string why;
};

struct PeerCaps {
  bool red = false, twcc = false, nack = false;
  int max_bps = 256000;       // min(host cap, listener's preference)
};

class CongestionController {
 public:
  CongestionController();
  void on_sent(uint16_t twcc_seq, int64_t send_us, int bytes, bool retransmit = false);
  void on_twcc(const std::vector<TwccResult> &fb, int64_t now_us);
  void on_rtt(double rtt_ms, int64_t now_us);
  void on_rr_loss(double fraction, int64_t now_us);
  void on_remb(uint32_t bps) { remb_bps_ = bps; }
  // Receiver-side view (from the listener page): buffer depth and residual
  // loss after all repair. Shapes redundancy, not the rate.
  void on_receiver(double jitter_buffer_ms, double residual_loss);

  Plan update(const PeerCaps &caps, int64_t now_us);  // cheap; call per frame

  // telemetry
  double target_bps() const { return target_; }
  double acked_bps() const { return acked_bps_; }
  double loss() const { return loss_.v; }
  double rtt_ms() const { return rtt_.init ? rtt_.v : 0; }
  double queue_ms() const { return queue_ms_; }
  double loss_bound() const { return loss_bound_; }
  const char *state() const;
  const Plan &plan() const { return plan_; }

 private:
  enum class Use { Normal, Over, Under };
  void trend_update(double delta_ms, double arrival_ms, int64_t now_us);
  void rate_update(int64_t now_us);
  void loss_update(double loss, int64_t now_us);

  struct Sent { int64_t send_us = -1; int bytes = 0; uint16_t seq = 0; bool retx = false; };
  std::vector<Sent> sent_;  // ring indexed by twcc seq

  // packet grouping
  bool have_group_ = false, have_prev_ = false;
  int64_t g_first_send_ = 0, g_last_send_ = 0, g_last_arr_ = 0;
  int64_t p_last_send_ = 0, p_last_arr_ = 0;
  int64_t first_arrival_ = -1;

  // trendline
  double acc_ = 0, smoothed_ = 0, threshold_ = 12.5, prev_trend_ = 0, time_over_ = -1;
  int over_count_ = 0, num_deltas_ = 0;
  int64_t last_thr_update_ = 0;
  std::deque<std::pair<double, double>> hist_;
  Use use_ = Use::Normal;

  // acked rate
  std::deque<std::pair<int64_t, int>> acked_;
  double acked_bps_ = 0;

  // rate control
  double target_ = 64000, loss_bound_ = 1e9;  // open at a rate nearly any link carries, then climb fast
  int64_t last_rate_us_ = 0, last_decrease_us_ = 0, start_us_ = 0;
  bool had_overuse_ = false;
  double cap_mean_ = -1, cap_var_ = 0.4;

  // loss
  Ewma loss_, rtt_;
  int64_t loss_window_start_ = 0;
  int win_lost_ = 0, win_total_ = 0;
  double rr_loss_ = 0;
  bool have_twcc_ = false;
  int64_t last_twcc_us_ = 0;

  // rtt fallback
  std::deque<std::pair<int64_t, int64_t>> owd_min_;  // monotonic deque: windowed minimum one-way delay
  std::deque<std::pair<int64_t, int64_t>> owd_short_;
  std::deque<std::pair<int64_t, double>> rtt_hist_;
  double min_rtt_ = 1e9, queue_ms_ = 0;

  uint32_t remb_bps_ = 0;
  double rx_buffer_ms_ = 0, rx_residual_ = 0;

  Plan plan_;
  int64_t last_shape_change_ = 0;
};

// Pure mapping from an allowed wire rate + observed conditions to encoder
// settings, with hysteresis around the previous plan. Exposed for tests.
Plan make_plan(double target_bps, double loss, double rtt_ms, double rx_buffer_ms, double residual,
               const PeerCaps &caps, const Plan &prev, bool allow_shape_change);

}  // namespace aoi
