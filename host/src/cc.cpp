#include "cc.h"

#include <algorithm>
#include <cmath>

#include "util.h"

namespace aoi {

namespace {
constexpr int kRing = 8192;
constexpr int kMinBps = 12000;
constexpr int64_t kGroupUs = 5000;     // packets sent within 5 ms form one group
constexpr size_t kTrendWindow = 20;
constexpr double kTrendGain = 4.0;
}  // namespace

CongestionController::CongestionController() : sent_(kRing) {
  start_us_ = now_us();
  last_rate_us_ = start_us_;
  loss_window_start_ = start_us_;
}

const char *CongestionController::state() const {
  if (use_ == Use::Over) return "congested";
  if (use_ == Use::Under) return "draining";
  if (!had_overuse_ && now_us() - start_us_ < 8000000) return "probing";
  return "steady";
}

void CongestionController::on_sent(uint16_t seq, int64_t send_us, int bytes, bool retx) {
  Sent &s = sent_[seq % kRing];
  s.send_us = send_us;
  s.bytes = bytes;
  s.seq = seq;
  s.retx = retx;
}

void CongestionController::on_twcc(const std::vector<TwccResult> &fb, int64_t now) {
  have_twcc_ = true;
  last_twcc_us_ = now;
  for (const TwccResult &r : fb) {
    const Sent &s = sent_[r.seq % kRing];
    if (s.send_us < 0 || s.seq != r.seq) continue;
    // Retransmissions go out right after a loss, into the same burst, and
    // would count the one loss twice: loss is judged on originals only.
    if (!s.retx) {
      ++win_total_;
      if (!r.received) ++win_lost_;
    }
    if (!r.received) continue;
    acked_.push_back({r.recv_us, s.bytes});
    if (first_arrival_ < 0) first_arrival_ = r.recv_us;
    // Queueing delay, directly: one-way delay (with an unknown clock offset)
    // above its minimum over the last 10 s. Unlike the trendline's running
    // sum it returns to zero when the queue drains.
    // Minimum over a short window vs minimum over a long one (LEDBAT): the
    // short minimum filters out jitter, which is delay *variance*, not a queue.
    {
      int64_t owd = r.recv_us - s.send_us;
      auto push = [&](std::deque<std::pair<int64_t, int64_t>> &q, int64_t span) {
        while (!q.empty() && q.back().second >= owd) q.pop_back();
        q.push_back({r.recv_us, owd});
        while (q.front().first < r.recv_us - span) q.pop_front();
      };
      push(owd_min_, 10000000);
      push(owd_short_, 1000000);
      queue_ms_ = std::max(0.0, double(owd_short_.front().second - owd_min_.front().second) / 1000.0);
    }

    // Group bursts; compare consecutive groups' send and arrival spacing.
    if (!have_group_) {
      have_group_ = true;
      g_first_send_ = g_last_send_ = s.send_us;
      g_last_arr_ = r.recv_us;
      continue;
    }
    if (s.send_us < g_first_send_) continue;  // reordered, belongs to an older group
    if (s.send_us - g_first_send_ <= kGroupUs) {
      g_last_send_ = std::max(g_last_send_, s.send_us);
      g_last_arr_ = std::max(g_last_arr_, r.recv_us);
      continue;
    }
    if (have_prev_) {
      int64_t ds = g_last_send_ - p_last_send_;
      int64_t da = g_last_arr_ - p_last_arr_;
      if (da >= 0) trend_update(double(da - ds) / 1000.0, double(g_last_arr_ - first_arrival_) / 1000.0, now);
    }
    have_prev_ = true;
    p_last_send_ = g_last_send_;
    p_last_arr_ = g_last_arr_;
    g_first_send_ = g_last_send_ = s.send_us;
    g_last_arr_ = r.recv_us;
  }
  // acked bitrate over the last 600 ms of arrivals
  if (!acked_.empty()) {
    int64_t newest = acked_.back().first;
    while (!acked_.empty() && newest - acked_.front().first > 600000) acked_.pop_front();
    if (acked_.size() >= 4) {
      int64_t span = std::max<int64_t>(acked_.back().first - acked_.front().first, 100000);
      double bytes = 0;
      for (auto &a : acked_) bytes += a.second;
      acked_bps_ = bytes * 8e6 / double(span);
    }
  }
  // loss over ~1 s windows
  // At 60 ms frames a second is only ~17 packets, and 3 % random loss reads
  // as 0 % or 18 % depending on luck. Judge on at least 50 packets.
  if (now - loss_window_start_ > 1000000 && win_total_ >= 50) {
    double l = double(win_lost_) / win_total_;
    loss_.add(l, 0.35);
    win_lost_ = win_total_ = 0;
    loss_window_start_ = now;
    loss_update(loss_.v, now);
  }
  rate_update(now);
}

// Loss-based bound, once per ~1 s window. Heavy loss (>10%) cuts; light loss
// (<2%) lets it recover; in between it holds -- moderate loss on a wireless
// hop is often random rather than congestion, and redundancy handles it.
void CongestionController::loss_update(double l, int64_t now) {
  // Loss with no queue building is a lossy hop (Wi-Fi, mobile), not a full
  // one: cutting the rate would not reduce it, only the quality. Redundancy
  // is the answer there. Cut on loss only with evidence of a queue, or when
  // loss is so heavy that nothing else will do.
  bool queue_evidence = use_ == Use::Over || queue_ms_ > 30 || now - last_decrease_us_ < 2000000;
  if (l > 0.25 || (l > 0.10 && queue_evidence)) {
    double base = acked_bps_ > 0 ? std::min(acked_bps_, target_) : target_;
    loss_bound_ = std::min(loss_bound_, base * (1 - 0.5 * l));
  } else if (loss_bound_ < 1e9 && (l < 0.02 || (l < 0.15 && !queue_evidence))) {
    loss_bound_ *= 1.08;
    if (loss_bound_ > target_ * 1.5) loss_bound_ = 1e9;
  }
}

void CongestionController::trend_update(double delta_ms, double arrival_ms, int64_t now) {
  ++num_deltas_;
  acc_ += delta_ms;
  smoothed_ = 0.9 * smoothed_ + 0.1 * acc_;
  hist_.push_back({arrival_ms, smoothed_});
  if (hist_.size() > kTrendWindow) hist_.pop_front();
  double trend = prev_trend_;
  if (hist_.size() >= 12) {  // judge before the full window: a queue growing for 20 samples has already hurt
    double mx = 0, my = 0;
    for (auto &h : hist_) { mx += h.first; my += h.second; }
    mx /= double(hist_.size()); my /= double(hist_.size());
    double num = 0, den = 0;
    for (auto &h : hist_) { num += (h.first - mx) * (h.second - my); den += (h.first - mx) * (h.first - mx); }
    if (den > 0) trend = num / den;
  }
  double modified = std::min(num_deltas_, 60) * trend * kTrendGain;
  // adaptive threshold (GCC): tracks the modified trend so a noisy path does
  // not look congested, and a quiet one reacts early.
  int64_t dt_ms = last_thr_update_ ? std::min<int64_t>((now - last_thr_update_) / 1000, 100) : 0;
  last_thr_update_ = now;
  if (std::fabs(modified) < threshold_ + 15.0) {
    double k = std::fabs(modified) < threshold_ ? 0.039 : 0.0087;
    threshold_ = std::clamp(threshold_ + k * (std::fabs(modified) - threshold_) * double(dt_ms), 6.0, 600.0);
  }
  if (modified > threshold_) {
    time_over_ = time_over_ < 0 ? 10.0 : time_over_ + 20.0;
    ++over_count_;
    if (time_over_ > 10.0 && over_count_ > 1 && trend >= prev_trend_) {
      use_ = Use::Over;
      time_over_ = 0;
      over_count_ = 0;
    }
  } else if (modified < -threshold_) {
    time_over_ = -1; over_count_ = 0;
    use_ = Use::Under;
  } else {
    time_over_ = -1; over_count_ = 0;
    use_ = Use::Normal;
  }
  prev_trend_ = trend;
}

void CongestionController::on_rtt(double rtt_ms, int64_t now) {
  if (rtt_ms <= 0 || rtt_ms > 10000) return;
  rtt_.add(rtt_ms, 0.2);
  rtt_hist_.push_back({now, rtt_ms});
  while (!rtt_hist_.empty() && now - rtt_hist_.front().first > 30000000) rtt_hist_.pop_front();
  min_rtt_ = 1e9;
  for (auto &r : rtt_hist_) min_rtt_ = std::min(min_rtt_, r.second);
  // Without transport feedback, queueing shows up as RTT above the path minimum.
  if (!have_twcc_ || now - last_twcc_us_ > 3000000) {
    queue_ms_ = std::max(0.0, rtt_.v - min_rtt_);
    use_ = queue_ms_ > std::max(60.0, min_rtt_ * 0.5) ? Use::Over : Use::Normal;
    rate_update(now);
  }
}

void CongestionController::on_rr_loss(double fraction, int64_t now) {
  rr_loss_ = fraction;
  if (!have_twcc_ || now - last_twcc_us_ > 3000000) {
    loss_.add(fraction, 0.3);
    loss_update(fraction, now);
    rate_update(now);
  }
}

void CongestionController::on_receiver(double buffer_ms, double residual) {
  rx_buffer_ms_ = buffer_ms;
  rx_residual_ = residual;
}

void CongestionController::rate_update(int64_t now) {
  double dt = std::clamp(double(now - last_rate_us_) / 1e6, 0.0, 1.0);
  last_rate_us_ = now;
  double rtt = rtt_.init ? rtt_.v : 100.0;

  // ---- delay-based AIMD ----
  if (use_ == Use::Over) {
    if (now - last_decrease_us_ > int64_t(std::max(rtt, 100.0) * 1000)) {
      double measured = acked_bps_ > 0 ? acked_bps_ : target_;
      double next = std::min(target_, 0.85 * measured);
      // remember where the link gave out: increase gently near it later
      if (cap_mean_ < 0) cap_mean_ = measured;
      else {
        double err = (measured - cap_mean_) / std::max(cap_mean_, 1.0);
        cap_var_ = std::clamp(0.95 * cap_var_ + 0.05 * err * err, 0.01, 0.4);
        cap_mean_ = 0.95 * cap_mean_ + 0.05 * measured;
      }
      target_ = next;
      last_decrease_us_ = now;
      had_overuse_ = true;
    }
  } else if (use_ == Use::Normal) {
    bool near_cap = cap_mean_ > 0 && std::fabs(target_ - cap_mean_) < 3 * std::sqrt(cap_var_) * cap_mean_;
    if (!had_overuse_ && now - start_us_ < 10000000) target_ *= std::pow(1.5, dt);  // startup: find the ceiling fast
    else if (near_cap) target_ += 4000.0 * dt;                                       // additive near the known limit
    else target_ *= std::pow(1.08, dt);                                              // multiplicative elsewhere
    // Audio is application limited: never let the estimate run away from
    // what has actually been delivered, or the first real test of it fails.
    if (acked_bps_ > 0) target_ = std::min(target_, std::max(1.5 * acked_bps_, acked_bps_ + 32000.0));
  }  // Under: hold while the queue drains

  if (loss_bound_ < 1e9) target_ = std::min(target_, loss_bound_);
  if (remb_bps_ > 0) target_ = std::min(target_, double(remb_bps_));
  target_ = std::clamp(target_, double(kMinBps), 600000.0);
}

Plan make_plan(double target, double loss, double rtt, double rx_buffer_ms, double residual, const PeerCaps &caps,
               const Plan &prev, bool allow_shape) {
  Plan p;
  target = std::min(target, double(caps.max_bps) * 1.35);  // headroom for redundancy above the Opus cap
  p.target_bps = int(target);

  // Frame duration: at low rates the 60-odd bytes of IP/UDP/SRTP/RTP header
  // per packet are a large share of the budget -- 25 kbps at 20 ms frames.
  // Longer frames buy that back when the link is thin.
  int frame = prev.frame_ms ? prev.frame_ms : 20;
  if (allow_shape) {
    if (target < 30000) frame = 60;
    else if (target < 52000) frame = 40;
    else if (target > 60000) frame = 20;
    else if (frame == 60) frame = 40;
  }
  p.frame_ms = frame;

  // Redundancy from the loss the network shows (before repair). NACK can
  // repair isolated losses if a retransmission lands before the listener's
  // buffer needs it, so a comfortable buffer means one level less RED.
  int red = 0;
  if (caps.red) {
    // One copy already turns isolated loss p into ~p^2; more only pays off
    // once loss is heavy or bursty.
    if (loss >= 0.20) red = 3;
    else if (loss >= 0.08) red = 2;
    else if (loss >= 0.01) red = 1;
    // Hysteresis: step protection down only once loss is clearly lower, so a
    // link hovering at a threshold does not flip the listener's quality.
    static const double down_at[4] = {0, 0.003, 0.05, 0.14};
    if (prev.opus_bps && prev.red > red && loss > down_at[prev.red]) red = prev.red;
    bool nack_in_time = caps.nack && rtt > 0 && rx_buffer_ms > 0 && rtt * 1.5 + 20 < rx_buffer_ms;
    if (nack_in_time && loss < 0.03 && red > 0) --red;
    if (residual > 0.01) red = std::min(3, red + 1);  // the listener is still concealing: add protection
  }
  // Retransmissions come out of the same budget.
  double budget = target * (1.0 - std::min(loss, 0.3) * (caps.nack ? 1.0 : 0.0));
  double pps = 1000.0 / frame;
  int opus = 0;
  for (;;) {
    double overhead = pps * 8.0 * (62 + (red ? 1 + 4 * red : 0));
    opus = int((budget - overhead) / (1 + red));
    // Prefer continuity to fidelity, but not below the point where Opus
    // stops being music: drop redundancy before starving the primary.
    if ((red >= 2 && opus < 14000) || (red == 1 && opus < 10000)) { --red; continue; }
    break;
  }
  opus = std::clamp(opus, 6000, caps.max_bps);
  p.red = red;
  p.opus_bps = opus;

  // Stereo folds to mono only when the bits are genuinely short; hysteresis
  // so it does not flap on a link hovering at the threshold.
  bool stereo = prev.opus_bps ? prev.stereo : true;
  if (allow_shape) {
    if (opus < 40000) stereo = false;
    else if (opus > 52000) stereo = true;
  }
  p.stereo = stereo;
  p.fec = loss > 0.01 && opus <= 48000;
  p.loss_pct = int(std::clamp(loss * 100.0 * 1.5, 0.0, 30.0));

  char why[160];
  snprintf(why, sizeof why, "%.0f kbps allowed, %.1f%% loss, %.0f ms rtt", target / 1000, loss * 100, rtt);
  p.why = why;
  return p;
}

Plan CongestionController::update(const PeerCaps &caps, int64_t now) {
  if (!have_twcc_) rate_update(now);  // keep startup/probing moving
  target_ = std::min(target_, double(caps.max_bps) * 1.35);
  bool shape = now - last_shape_change_ > 4000000;
  Plan p = make_plan(target_, loss_.init ? loss_.v : 0, rtt_.init ? rtt_.v : 0, rx_buffer_ms_, rx_residual_, caps,
                     plan_, shape);
  if (p.frame_ms != plan_.frame_ms || p.stereo != plan_.stereo) last_shape_change_ = now;
  plan_ = p;
  return p;
}

}  // namespace aoi
