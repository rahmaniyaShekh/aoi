// The return channel: one listener's microphone, received, buffered, decoded.
//
// A small NetEq: packets are held by RTP timestamp; playout decodes in order,
// recovers a missing frame from the next packet's in-band FEC when it has one,
// conceals with Opus PLC otherwise, and adapts its target delay to the jitter
// it measures. The listener's sound card and ours run off different crystals,
// so the decoded audio is read at a servo-controlled fractional rate that
// holds the total delay at its target -- drift becomes an inaudible +-0.5 %
// rate trim instead of a buffer that slowly empties or fills.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "dsp.h"
#include "rtp.h"

struct OpusDecoder;

namespace aoi {

class TalkStream {
 public:
  TalkStream();
  ~TalkStream();
  TalkStream(const TalkStream &) = delete;
  TalkStream &operator=(const TalkStream &) = delete;

  void push(const RtpPacketInfo &rtp, int64_t arrival_us);
  void on_sender_report(uint64_t ntp, int64_t arrival_us);
  // Render thread: adds n mono 48 kHz samples * gain into out.
  void mix_into(float *out, int n, float gain);

  bool active() const;              // audio received recently
  bool report(ReportBlock &rb);     // RTCP receiver report for the talker's stream
  std::vector<uint16_t> take_nacks(int64_t now_us, double rtt_ms);
  uint32_t ssrc() const { return ssrc_; }

  struct Stats { double jitter_ms, target_ms, depth_ms, level_db; uint64_t lost, concealed, fec; bool playing; };
  Stats stats() const;

 private:
  void decode_one(std::vector<float> &out);
  int64_t unwrap_ts(uint32_t ts);

  mutable std::mutex mu_;
  OpusDecoder *dec_ = nullptr;
  std::map<int64_t, std::vector<uint8_t>> pkts_;  // extended timestamp -> opus frame
  FracRing ring_{kRate};
  bool playing_ = false, have_ts_ = false;
  int64_t last_ext_ts_ = 0, next_ts_ = 0;
  int last_frame_ = 960, conceal_run_ = 0;
  uint32_t ssrc_ = 0;

  // arrival statistics
  bool have_seq_ = false;
  uint16_t max_seq_ = 0;
  uint32_t cycles_ = 0, base_seq_ = 0, received_ = 0, expected_prior_ = 0, received_prior_ = 0;
  double jitter_ = 0;  // RFC 3550, in timestamp units
  int64_t last_transit_ = 0;
  bool have_transit_ = false;
  double jitter_peak_ms_ = 0;
  double target_ms_ = 60, underrun_boost_ms_ = 0;
  int64_t last_arrival_ = 0, last_decay_ = 0;
  uint64_t lost_ = 0, concealed_ = 0, fec_ = 0;
  float level_db_ = -90;

  // sender report bookkeeping for RR
  uint32_t lsr_ = 0;
  int64_t lsr_arrival_ = 0;

  // NACK state: seq -> (first seen missing, times sent)
  struct Missing { int64_t since; int64_t last_sent; int sent; };
  std::map<uint16_t, Missing> missing_;
};

}  // namespace aoi
