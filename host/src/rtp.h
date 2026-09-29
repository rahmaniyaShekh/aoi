// RTP/RTCP, built and parsed by hand.
//
// We packetize ourselves (rather than use a library packetizer) because the
// loss-resilience features that matter on a bad link all live here:
//  * RED (RFC 2198): each packet can carry copies of the previous 1-3 Opus
//    frames, so a lost packet is rebuilt from the next one with no round trip.
//  * NACK (RFC 4585): lost packets the receiver can still use are resent.
//  * Transport-wide sequence numbers + TWCC feedback: per-packet arrival
//    times, which drive the delay-based congestion controller.
//  * Sender reports, so receiver reports carry a usable RTT.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoi {

constexpr int kPtOpus = 111;
constexpr int kPtRed = 63;
constexpr int kExtTwcc = 3;  // extmap id for transport-wide-cc
constexpr int kRtpHeader = 12;

struct RtpPacketInfo {
  uint8_t pt = 0;
  bool marker = false;
  uint16_t seq = 0;
  uint32_t ts = 0, ssrc = 0;
  const uint8_t *payload = nullptr;
  size_t payload_len = 0;
  std::optional<uint16_t> twcc_seq;
};
bool parse_rtp(const uint8_t *p, size_t n, RtpPacketInfo &out, int twcc_ext_id = kExtTwcc);

// One Opus frame as it went out, kept for RED and retransmission.
struct Frame {
  uint32_t ts = 0;
  std::vector<uint8_t> data;
};

// Builds a full RTP packet (header + TWCC extension + payload).
std::vector<uint8_t> build_rtp(uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc, bool marker,
                               uint16_t twcc_seq, const uint8_t *payload, size_t len);
// Patches the transport-wide sequence number in a packet built by build_rtp.
void set_twcc_seq(std::vector<uint8_t> &pkt, uint16_t twcc_seq);

// RED payload: `redundant` are older frames (oldest first), then the primary.
std::vector<uint8_t> build_red(const std::vector<const Frame *> &redundant, const Frame &primary, uint8_t block_pt);
struct RedBlock { uint32_t ts; const uint8_t *data; size_t len; uint8_t pt; };
bool parse_red(const uint8_t *p, size_t n, uint32_t primary_ts, std::vector<RedBlock> &out);

// ---- RTCP -----------------------------------------------------------------------------
struct ReportBlock {
  uint32_t ssrc = 0;
  uint8_t fraction_lost = 0;
  int32_t cumulative_lost = 0;
  uint32_t highest_seq = 0, jitter = 0, lsr = 0, dlsr = 0;
};
struct TwccResult {
  uint16_t seq;
  bool received;
  int64_t recv_us;  // valid if received; relative to an arbitrary receiver epoch
};
struct RtcpEvents {
  std::vector<ReportBlock> reports;      // RR/SR blocks about our stream
  std::vector<uint16_t> nacks;           // sequence numbers asked for
  std::vector<TwccResult> twcc;          // in feedback order
  std::optional<uint32_t> remb_bps;
  bool has_sr = false; uint32_t sr_ssrc = 0; uint64_t sr_ntp = 0;  // the peer's sender report
  int bye = 0;
};
bool is_rtcp(const uint8_t *p, size_t n);
void parse_rtcp(const uint8_t *p, size_t n, uint32_t our_ssrc, RtcpEvents &ev);
bool parse_twcc(const uint8_t *fci, size_t n, std::vector<TwccResult> &out);

std::vector<uint8_t> build_sr(uint32_t ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packets, uint32_t octets);
std::vector<uint8_t> build_rr(uint32_t our_ssrc, const ReportBlock &rb);
std::vector<uint8_t> build_nack(uint32_t our_ssrc, uint32_t media_ssrc, const std::vector<uint16_t> &seqs);
// Test helper: encodes TWCC feedback (used by the self test and the simulator).
std::vector<uint8_t> build_twcc(uint32_t sender_ssrc, uint32_t media_ssrc, uint16_t base_seq,
                                const std::vector<std::optional<int64_t>> &recv_us, uint8_t fb_count);

}  // namespace aoi
