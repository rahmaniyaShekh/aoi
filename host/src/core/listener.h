// One connected listener: a WebRTC peer with its own mix, encoder and
// congestion controller.
//
// Everything that adapts is per listener. A phone on a train and a laptop on
// fibre watching the same host each get the best stream *their* path can
// carry, and each listener sets their own balance between the host's music
// and the host's voice. Both are mixed into one stream here, on one clock, so
// voice and music can never drift apart at the far end and the balance costs
// no extra bandwidth.
#pragma once
#include <rtc/rtc.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cc.h"
#include "dsp.h"
#include "netsim.h"
#include "rendezvous.h"
#include "talkback.h"
#include "util.h"

struct OpusEncoder;

namespace aoi {

// One 10 ms block of source audio, shared read-only by every listener.
struct SourceBlock {
  std::array<float, kBlock * 2> system{};  // stereo, after auto-level
  std::array<float, kBlock * 2> system_raw{};  // stereo, as captured (split mode sends it untouched)
  std::array<float, kBlock> voice{};       // host mic, conditioned (zeros when off)
  bool voice_active = false;
  bool mic_on = false;
};

// What the listener asked for from their page.
struct MixPrefs {
  float music_db = 0.f, voice_db = 0.f;
  bool music_mute = false, voice_mute = false;
  bool duck = true;
  int max_kbps = 256;
};

struct ListenerView {
  int id = 0;
  std::string name, state, path, why, cc_state;
  double rtt_ms = 0, loss = 0, queue_ms = 0, target_kbps = 0, opus_kbps = 0, wire_kbps = 0;
  int frame_ms = 20, red = 0;
  bool stereo = true, fec = false, twcc = false, nack = false, redcap = false;
  double rx_buffer_ms = 0, rx_conceal = 0;
  uint64_t retransmits = 0, packets = 0;
  bool talking = false;
  double talk_level_db = -90;
  MixPrefs prefs;
  int quality = 0;   // 0..4 bars
  double connected_s = 0;
};

// Where relay messages travel: the host's own signalling socket (no second
// connection to open), or a WebSocket of their own.
class RelayPipe {
 public:
  virtual ~RelayPipe() = default;
  virtual bool send(const uint8_t *p, size_t n) = 0;  // false: the relay is gone
  virtual std::optional<std::string> receive() = 0;   // blocks; nullopt once closed
  virtual void close() = 0;
  virtual const char *kind() const = 0;
};

class Listener : public std::enable_shared_from_this<Listener> {
 public:
  enum class State { Offering, Connecting, Live, Closed };

  struct Env {
    std::vector<IceServerCfg> ice;
    NetSimConfig sim;
    std::string host_name;
    std::atomic<bool> *talk_allowed;
    std::atomic<bool> *mic_on;
    std::atomic<bool> *mic_active = nullptr;
    std::atomic<int> *host_max_kbps;
    std::atomic<int> *listener_count;
    // Split mode: the host's voice in the left channel, the PC's sound in the
    // right, unmixed; the listener's page mixes them (or keeps them apart).
    bool split = false;
  };

  Listener(int id, Env env);
  ~Listener();

  // Signalling. create_offer blocks until ICE gathering completes (or the
  // timeout), because the whole handshake is one sealed offer + one answer.
  std::string create_offer(int gather_timeout_ms, const std::atomic<bool> *cancel = nullptr);
  bool set_answer(const std::string &sdp, std::string *err);
  // Relay instead of a direct WebRTC path: the listener asked for it because
  // its network could not reach ours. `url` is the relay WebSocket endpoint.
  // `rdv`/`session`: relay over the host's signalling socket when the server
  // supports it; otherwise (or on failure) over a connection of its own.
  bool start_relay(const std::string &url, const std::vector<uint8_t> &key, std::string *err,
                   std::weak_ptr<Rendezvous> rdv = {}, const std::string &session = {});
  bool relayed() const { return relay_; }
  void close(const std::string &reason);
  void shutdown();  // engine thread only

  void push(std::shared_ptr<const SourceBlock> b);   // capture thread, never blocks
  void mix_talk(float *out, int n, float gain);      // render thread

  State state() const { return state_; }
  bool dead() const;
  int id() const { return id_; }
  double age_s() const { return (now_us() - created_us_) / 1e6; }
  bool was_live() const { return live_since_ != 0; }
  ListenerView view() const;
  void notify_settings();  // host toggled mic/talkback: tell the page

 private:
  void sender_loop();
  void process_block(const SourceBlock &b);
  void encode_frame();
  void send_packet(std::vector<uint8_t> &&pkt, bool media);
  void send_voice_packet(std::vector<uint8_t> &&pkt);
  void on_rtp_or_rtcp(const rtc::binary &msg);
  void on_control(const std::string &text);
  void send_ctl(const std::string &json);
  void send_stats();
  void log_summary();
  void apply_plan(const Plan &p);

  const int id_;
  Env env_;
  const int64_t created_us_;
  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> track_;
  std::shared_ptr<rtc::DataChannel> dc_;
  std::unique_ptr<NetSim> sim_;
  std::atomic<State> state_{State::Offering};
  std::atomic<bool> closing_{false}, dc_open_{false}, shut_{false}, media_up_{false};
  std::atomic<int64_t> last_ctl_us_{0}, disconnected_since_{0}, live_since_{0}, answered_us_{0};
  std::string close_reason_;

  // sender thread
  std::thread thread_;
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<std::shared_ptr<const SourceBlock>> queue_;
  bool stop_ = false;

  // mixing + encoding (sender thread only)
  Limiter limiter_{-1.0f, 5.f, 150.f};
  float g_music_ = 1.f, g_voice_ = 1.f;
  std::vector<float> pcm_;  // pending interleaved stereo for the next frame
  OpusEncoder *enc_ = nullptr;
  Plan applied_;
  uint32_t rtp_ts_ = 0;
  bool first_packet_ = true;

  // RTP state (guarded by mu_: sender thread + network callbacks)
  mutable std::mutex mu_;
  uint32_t ssrc_;
  uint16_t seq_ = 0, twcc_seq_ = 0;
  std::deque<Frame> recent_;                      // for RED
  struct Hist { uint16_t seq = 0; int64_t sent_us = 0, resent_us = 0; std::vector<uint8_t> pkt; };
  std::vector<Hist> history_;                     // ring by seq, for NACK
  CongestionController cc_;
  PeerCaps caps_;
  MixPrefs prefs_;
  uint32_t packets_ = 0, octets_ = 0;
  uint64_t retransmits_ = 0;
  double resend_budget_ = 0;
  int64_t last_budget_us_ = 0;
  std::deque<std::pair<int64_t, int>> wire_log_;  // (time, bytes) for wire kbps
  double rx_buffer_ms_ = 0, rx_conceal_ = 0, rx_rtt_ms_ = 0;
  std::string rx_path_, name_;
  int diag_ = 0;
  int64_t last_sr_us_ = 0, last_stat_us_ = 0, last_rr_us_ = 0, last_nack_us_ = 0;

  TalkStream talk_;

  // ---- split mode: the host's voice as a stream of its own ----
  // Its own Opus encoder (speech mode, mono), RTP stream (own SSRC, sequence
  // and timestamps, sharing the transport-wide sequence) and NACK history.
  // Over WebRTC it is a second track; over the relay it rides in the same
  // message as the PC's sound, frame by frame. Never mixed with the sound.
  OpusEncoder *venc_ = nullptr;
  std::shared_ptr<rtc::Track> vtrack_;
  uint32_t vssrc_ = 0, vrtp_ts_ = 0, vpackets_ = 0, voctets_ = 0;
  uint16_t vseq_ = 0;
  bool vfirst_ = true;
  int vbps_ = 32000, applied_vbps_ = 0, vloss_ = -1;
  std::vector<float> vpcm_;   // pending mono voice, in step with pcm_
  std::vector<Hist> vhistory_;
  // A compound RTCP packet naming both of our SSRCs reaches both tracks:
  // count it once.
  std::mutex rtcp_mu_;
  std::deque<uint64_t> rtcp_seen_;

  // ---- relay mode ----
  void relay_loop();
  void relay_tx_loop();
  void relay_send(uint8_t type, const uint8_t *p, size_t n);
  void relay_frame(uint32_t ts, const uint8_t *p, size_t n, int frame_ms, const uint8_t *v = nullptr, size_t vn = 0);
  Plan relay_plan();
  std::atomic<bool> relay_{false};
  std::vector<uint8_t> relay_key_;
  std::unique_ptr<RelayPipe> pipe_;
  std::thread relay_thread_, relay_tx_thread_;
  // Sealed messages waiting for the socket. Sending blocks while TCP is
  // full, so it has a thread of its own: the encoder never waits, stale
  // audio is dropped instead of queued, and the backlog is the congestion
  // signal for the relay bitrate.
  struct RelayMsg { bool audio; int ms; std::vector<uint8_t> data; };
  std::mutex rq_mu_;
  std::condition_variable rq_cv_;
  std::deque<RelayMsg> rq_;
  int rq_audio_ms_ = 0;
  bool rq_stop_ = false;
  std::atomic<int> relay_backlog_ms_{0};
  std::atomic<uint64_t> relay_dropped_ms_{0};
  std::vector<uint8_t> relay_batch_;
  int relay_batch_frames_ = 0, relay_batch_ms_ = 0;
  std::atomic<int> relay_kbps_{64};
  int64_t relay_adapt_us_ = 0, relay_clean_since_ = 0, relay_cut_us_ = 0;
  Ewma relay_send_ms_;
  uint16_t relay_talk_seq_ = 0;
  std::atomic<bool> relay_heard_{false};
};

}  // namespace aoi
