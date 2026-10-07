#include "talkback.h"

#include <opus.h>

#include <algorithm>
#include <cmath>

#include "util.h"

namespace aoi {

TalkStream::TalkStream() {
  int err = 0;
  dec_ = opus_decoder_create(kRate, 1, &err);
}

TalkStream::~TalkStream() {
  if (dec_) opus_decoder_destroy(dec_);
}

int64_t TalkStream::unwrap_ts(uint32_t ts) {
  if (!have_ts_) { have_ts_ = true; last_ext_ts_ = ts; return ts; }
  int32_t d = int32_t(ts - uint32_t(last_ext_ts_));
  int64_t ext = last_ext_ts_ + d;
  if (d > 0) last_ext_ts_ = ext;
  return ext;
}

void TalkStream::push(const RtpPacketInfo &rtp, int64_t arrival) {
  std::lock_guard lk(mu_);
  ssrc_ = rtp.ssrc;
  last_arrival_ = arrival;

  // --- sequence bookkeeping (loss, RR, NACK) ---
  if (!have_seq_) {
    have_seq_ = true;
    max_seq_ = rtp.seq;
    base_seq_ = rtp.seq;
  } else if (seq_newer(rtp.seq, max_seq_)) {
    uint16_t gap = uint16_t(rtp.seq - max_seq_);
    if (gap > 1 && gap < 200)
      for (uint16_t s = uint16_t(max_seq_ + 1); s != rtp.seq; ++s) missing_[s] = {arrival, 0, 0};
    if (rtp.seq < max_seq_) ++cycles_;
    max_seq_ = rtp.seq;
  } else {
    missing_.erase(rtp.seq);  // a late or retransmitted packet filled a hole
  }
  ++received_;

  // --- interarrival jitter (RFC 3550) ---
  int64_t arrival_ts = arrival * kRate / 1000000;
  int64_t transit = arrival_ts - int64_t(rtp.ts);
  if (have_transit_) {
    double d = double(std::llabs(transit - last_transit_));
    if (d < kRate) jitter_ += (d - jitter_) / 16.0;
  }
  have_transit_ = true;
  last_transit_ = transit;
  double jms = jitter_ * 1000.0 / kRate;
  if (arrival - last_decay_ > 1000000) {
    jitter_peak_ms_ *= 0.9;  // the peak forgets slowly
    underrun_boost_ms_ = std::max(0.0, underrun_boost_ms_ - 2.0);
    last_decay_ = arrival;
  }
  jitter_peak_ms_ = std::max(jitter_peak_ms_, jms);

  // --- payload ---
  auto store = [&](int64_t ext, const uint8_t *d, size_t n) {
    if (n == 0 || n > 1500) return;
    if (playing_ && ext < next_ts_) return;  // too late to be useful
    pkts_.emplace(ext, std::vector<uint8_t>(d, d + n));
  };
  int64_t ext = unwrap_ts(rtp.ts);
  if (rtp.pt == kPtRed) {
    std::vector<RedBlock> blocks;
    if (parse_red(rtp.payload, rtp.payload_len, rtp.ts, blocks))
      for (auto &b : blocks) store(ext - int64_t(uint32_t(rtp.ts - b.ts)), b.data, b.len);
  } else {
    store(ext, rtp.payload, rtp.payload_len);
  }
  // Bound memory: nothing more than 2 s ahead is worth keeping.
  while (pkts_.size() > 150) pkts_.erase(pkts_.begin());

  target_ms_ = std::clamp(2.5 * jitter_peak_ms_ + last_frame_ * 1000.0 / kRate + 20.0 + underrun_boost_ms_, 40.0, 300.0);
}

void TalkStream::on_sender_report(uint64_t ntp, int64_t arrival) {
  std::lock_guard lk(mu_);
  lsr_ = uint32_t(ntp >> 16);
  lsr_arrival_ = arrival;
}

void TalkStream::decode_one(std::vector<float> &out) {
  out.assign(5760, 0.f);
  int n = 0;
  auto it = pkts_.find(next_ts_);
  if (it != pkts_.end()) {
    n = opus_decode_float(dec_, it->second.data(), opus_int32(it->second.size()), out.data(), 5760, 0);
    pkts_.erase(it);
    conceal_run_ = 0;
  } else {
    auto after = pkts_.upper_bound(next_ts_);
    if (after != pkts_.end() && after->first - next_ts_ > kRate / 2) {
      // A gap of half a second: nothing to conceal, jump to the audio we have.
      next_ts_ = after->first;
      decode_one(out);
      return;
    }
    if (after != pkts_.end() && after->first == next_ts_ + last_frame_) {
      // The next packet may carry this frame's low-bitrate copy (LBRR).
      n = opus_decode_float(dec_, after->second.data(), opus_int32(after->second.size()), out.data(), last_frame_, 1);
      ++fec_;
    } else {
      n = opus_decode_float(dec_, nullptr, 0, out.data(), last_frame_, 0);
    }
    concealed_ += uint64_t(std::max(n, 0));
    ++lost_;
    ++conceal_run_;
  }
  if (n <= 0) { n = last_frame_; std::fill(out.begin(), out.begin() + n, 0.f); }
  last_frame_ = n;
  next_ts_ += n;
  out.resize(size_t(n));
}

void TalkStream::mix_into(float *out, int n, float gain) {
  std::lock_guard lk(mu_);
  if (!dec_) return;
  if (!playing_) {
    if (pkts_.empty()) return;
    // Start once the buffer holds the target delay.
    int64_t span = pkts_.rbegin()->first - pkts_.begin()->first + last_frame_;
    if (span * 1000 / kRate < int64_t(target_ms_)) return;
    playing_ = true;
    next_ts_ = pkts_.begin()->first;
    conceal_run_ = 0;
    opus_decoder_ctl(dec_, OPUS_RESET_STATE);
    ring_.reset();
  }
  // Total delay = undecoded packets + decoded samples waiting to play.
  double queued = pkts_.empty() ? 0.0 : double(pkts_.rbegin()->first + last_frame_ - next_ts_);
  double depth = std::max(0.0, queued) + ring_.avail();
  double target = target_ms_ * kRate / 1000.0;
  if (depth > target + kRate * 0.2) {
    // Far behind (a burst after a stall): drop the oldest audio to catch up.
    int64_t keep_from = pkts_.rbegin()->first - int64_t(target);
    while (!pkts_.empty() && pkts_.begin()->first < keep_from) pkts_.erase(pkts_.begin());
    if (!pkts_.empty()) next_ts_ = pkts_.begin()->first;
    ring_.reset();
    depth = target;
  }
  double err = (depth - target) / std::max(target, 1.0);
  double ratio = 1.0 + std::clamp(0.005 * err, -0.005, 0.005);

  std::vector<float> dec;
  while (ring_.avail() < n * ratio + 4) {
    if (pkts_.empty() && conceal_run_ >= 3) break;  // talker went quiet: stop inventing audio
    decode_one(dec);
    ring_.write(dec.data(), int(dec.size()));
  }
  float tmp[4096];
  int total = 0;
  double ss = 0;
  while (total < n) {
    int chunk = std::min(n - total, int(std::size(tmp)));
    int got = ring_.read(tmp, chunk, ratio);
    for (int i = 0; i < got; ++i) { out[total + i] += tmp[i] * gain; ss += double(tmp[i]) * tmp[i]; }
    total += got;
    if (got < chunk) break;
  }
  level_db_ = 0.8f * level_db_ + 0.2f * gain_to_db(float(std::sqrt(ss / std::max(n, 1))));
  if (total < n) {
    // Ran dry: fall silent and re-buffer, a little deeper next time.
    playing_ = false;
    if (!pkts_.empty() || now_us() - last_arrival_ < 300000) underrun_boost_ms_ = std::min(underrun_boost_ms_ + 20.0, 150.0);
  }
}

bool TalkStream::active() const {
  std::lock_guard lk(mu_);
  return last_arrival_ && now_us() - last_arrival_ < 600000;
}

bool TalkStream::report(ReportBlock &rb) {
  std::lock_guard lk(mu_);
  if (!have_seq_) return false;
  uint32_t ext_max = (cycles_ << 16) | max_seq_;
  uint32_t expected = ext_max - base_seq_ + 1;
  uint32_t exp_int = expected - expected_prior_, rec_int = received_ - received_prior_;
  expected_prior_ = expected;
  received_prior_ = received_;
  int32_t lost_int = int32_t(exp_int) - int32_t(rec_int);
  rb.ssrc = ssrc_;
  rb.fraction_lost = (exp_int == 0 || lost_int <= 0) ? 0 : uint8_t(std::min<uint32_t>(255, (uint32_t(lost_int) << 8) / exp_int));
  rb.cumulative_lost = std::clamp<int32_t>(int32_t(expected) - int32_t(received_), -0x800000, 0x7FFFFF);
  rb.highest_seq = ext_max;
  rb.jitter = uint32_t(jitter_);
  rb.lsr = lsr_;
  rb.dlsr = lsr_arrival_ ? uint32_t((now_us() - lsr_arrival_) * 65536 / 1000000) : 0;
  return true;
}

std::vector<uint16_t> TalkStream::take_nacks(int64_t now, double rtt_ms) {
  std::lock_guard lk(mu_);
  std::vector<uint16_t> out;
  double useful_ms = target_ms_ - 10.0;
  for (auto it = missing_.begin(); it != missing_.end();) {
    double age_ms = double(now - it->second.since) / 1000.0;
    // Only ask while a retransmission could still arrive before playout.
    if (age_ms + rtt_ms > useful_ms || it->second.sent >= 2) { it = missing_.erase(it); continue; }
    if (it->second.sent == 0 || double(now - it->second.last_sent) / 1000.0 > std::max(rtt_ms, 20.0)) {
      out.push_back(it->first);
      it->second.sent++;
      it->second.last_sent = now;
    }
    ++it;
  }
  return out;
}

TalkStream::Stats TalkStream::stats() const {
  std::lock_guard lk(mu_);
  double queued = pkts_.empty() ? 0.0 : double(pkts_.rbegin()->first + last_frame_ - next_ts_);
  return {jitter_ * 1000.0 / kRate, target_ms_, (std::max(0.0, queued) + ring_.avail()) * 1000.0 / kRate,
          level_db_, lost_, concealed_, fec_, playing_};
}

}  // namespace aoi
