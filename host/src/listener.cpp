#include "listener.h"

#include <opus.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util.h"

using json = nlohmann::json;

namespace aoi {

namespace {
constexpr int kHistory = 1024;  // ~20 s of 20 ms packets, far more than any NACK needs

// "turn:host:port?transport=udp" -> rtc::IceServer. libjuice relays over UDP
// only, so TCP/TLS TURN URLs are skipped here (the browser still uses them).
bool to_ice(const IceServerCfg &c, rtc::IceServer &out) {
  std::string u = c.url;
  bool turn = u.rfind("turn:", 0) == 0, stun = u.rfind("stun:", 0) == 0;
  if (!turn && !stun) return false;
  std::string rest = u.substr(5), query;
  auto q = rest.find('?');
  if (q != std::string::npos) { query = rest.substr(q + 1); rest = rest.substr(0, q); }
  if (query.find("transport=tcp") != std::string::npos) return false;
  auto colon = rest.rfind(':');
  std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
  uint16_t port = colon == std::string::npos ? 3478 : uint16_t(atoi(rest.c_str() + colon + 1));
  if (host.empty() || port == 53) return false;
  if (stun) out = rtc::IceServer(host, port);
  else out = rtc::IceServer(host, port, c.username, c.credential, rtc::IceServer::RelayType::TurnUdp);
  return true;
}

// Section of an SDP belonging to the first audio m-line.
std::string audio_section(const std::string &sdp) {
  auto a = sdp.find("m=audio");
  if (a == std::string::npos) return {};
  auto b = sdp.find("\nm=", a + 1);
  return sdp.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
}  // namespace

Listener::Listener(int id, Env env) : id_(id), env_(std::move(env)), created_us_(now_us()), history_(kHistory) {
  ssrc_ = random_u32() | 1;
  seq_ = uint16_t(random_u32());
  rtp_ts_ = random_u32();
  int err = 0;
  enc_ = opus_encoder_create(kRate, 2, OPUS_APPLICATION_AUDIO, &err);
  if (enc_) {
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(1));  // bounded bursts: kinder to a thin bottleneck
    opus_encoder_ctl(enc_, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(enc_, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc_, OPUS_SET_LSB_DEPTH(24));
  }
  if (env_.sim.active())
    sim_ = std::make_unique<NetSim>(env_.sim, [this](std::vector<uint8_t> &&p) {
      auto t = track_;
      if (t && t->isOpen()) {
        try { t->send(reinterpret_cast<const std::byte *>(p.data()), p.size()); } catch (...) {}
      }
    });
  thread_ = std::thread([this] { sender_loop(); });
}

Listener::~Listener() {
  shutdown();
  if (enc_) opus_encoder_destroy(enc_);
}

// Called from the engine's own thread, never from a libdatachannel callback:
// closing a PeerConnection from inside one of its callbacks can deadlock.
void Listener::shutdown() {
  if (shut_.exchange(true)) return;
  {
    std::lock_guard lk(qmu_);
    stop_ = true;
  }
  qcv_.notify_all();
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  sim_.reset();
  if (pc_) {
    if (dc_) dc_->resetCallbacks();
    if (track_) track_->resetCallbacks();
    pc_->resetCallbacks();
    int64_t t0 = now_us();
    try { pc_->close(); } catch (...) {}
    LOGD("listener %d: peer closed in %lld ms", id_, (long long)((now_us() - t0) / 1000));
  }
  state_ = State::Closed;
}

std::string Listener::create_offer(int gather_timeout_ms, const std::atomic<bool> *cancel) {
  rtc::Configuration cfg;
  for (auto &s : env_.ice) {
    rtc::IceServer is("stun.cloudflare.com", 3478);
    if (to_ice(s, is)) cfg.iceServers.push_back(is);
  }
  cfg.disableAutoNegotiation = true;
  cfg.mtu = 1200;
  pc_ = std::make_shared<rtc::PeerConnection>(cfg);

  std::weak_ptr<Listener> weak = weak_from_this();
  auto gathered = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto first_srflx = std::make_shared<std::atomic<int64_t>>(0);
  pc_->onLocalCandidate([first_srflx](rtc::Candidate c) {
    if ((c.type() == rtc::Candidate::Type::ServerReflexive || c.type() == rtc::Candidate::Type::Relayed) && !*first_srflx)
      *first_srflx = now_us();
  });
  pc_->onGatheringStateChange([gathered, done](rtc::PeerConnection::GatheringState s) {
    if (s == rtc::PeerConnection::GatheringState::Complete) {
      *done = true;
      gathered->second.notify_all();
    }
  });
  pc_->onStateChange([weak](rtc::PeerConnection::State s) {
    auto self = weak.lock();
    if (!self) return;
    using S = rtc::PeerConnection::State;
    if (s == S::Connected) {
      self->disconnected_since_ = 0;
      LOGI("listener %d: connected", self->id_);
    } else if (s == S::Disconnected) {
      if (!self->disconnected_since_) self->disconnected_since_ = now_us();
    } else if (s == S::Failed || s == S::Closed) {
      if (self->state_ != State::Closed) LOGI("listener %d: peer %s", self->id_, s == S::Failed ? "failed" : "closed");
      self->state_ = State::Closed;
    }
  });

  rtc::Description::Audio media("0", rtc::Description::Direction::SendRecv);
  media.addOpusCodec(kPtOpus, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;maxaveragebitrate=510000;maxplaybackrate=48000");
  if (auto *m = media.rtpMap(kPtOpus)) {
    m->addFeedback("transport-cc");
    m->addFeedback("nack");
  }
  media.addAudioCodec(kPtRed, "red/48000/2", std::string("111/111"));
  media.addExtMap(rtc::Description::Entry::ExtMap(kExtTwcc,
                  "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
  media.addSSRC(ssrc_, std::string("aoi"), std::string("aoi-stream"), std::string("aoi-audio"));
  track_ = pc_->addTrack(media);
  track_->onMessage([weak](rtc::binary msg) { if (auto s = weak.lock()) s->on_rtp_or_rtcp(msg); },
                    [](rtc::string) {});

  rtc::DataChannelInit init;  // reliable, ordered: control is tiny and must arrive
  dc_ = pc_->createDataChannel("ctl", init);
  dc_->onOpen([weak] {
    auto s = weak.lock();
    if (!s) return;
    s->dc_open_ = true;
    s->last_ctl_us_ = now_us();
    s->live_since_ = now_us();
    s->state_ = State::Live;
    LOGI("listener %d: live", s->id_);
    s->notify_settings();
  });
  dc_->onMessage([weak](rtc::message_variant m) {
    auto s = weak.lock();
    if (!s) return;
    s->last_ctl_us_ = now_us();
    if (std::holds_alternative<std::string>(m)) s->on_control(std::get<std::string>(m));
  });
  dc_->onClosed([weak] { if (auto s = weak.lock()) s->state_ = State::Closed; });

  pc_->setLocalDescription(rtc::Description::Type::Offer);
  // Non-trickle: the offer must carry the candidates. Wait for gathering to
  // finish -- but a slow or filtered STUN server should not hold the offer
  // hostage: once a public candidate is in hand, give the rest one second.
  {
    const int64_t t0 = now_us();
    std::unique_lock lk(gathered->first);
    while (!done->load()) {
      if (cancel && cancel->load()) break;
      int64_t now = now_us();
      if (now - t0 > int64_t(gather_timeout_ms) * 1000) break;
      int64_t s = first_srflx->load();
      if (s && now - s > 1000000) break;
      gathered->second.wait_for(lk, std::chrono::milliseconds(100));
    }
  }
  auto desc = pc_->localDescription();
  int64_t s = first_srflx->load();
  LOGD("offer gathered: %s in %lld ms (first public candidate at %lld ms)", done->load() ? "complete" : "partial",
       (long long)((now_us() - created_us_) / 1000), (long long)(s ? (s - created_us_) / 1000 : -1));
  return desc ? std::string(*desc) : std::string();
}

bool Listener::set_answer(const std::string &sdp, std::string *err) {
  std::string a = audio_section(sdp);
  {
    std::lock_guard lk(mu_);
    caps_.red = a.find("a=rtpmap:63 red/48000") != std::string::npos;
    caps_.twcc = a.find("transport-wide-cc") != std::string::npos && a.find("transport-cc") != std::string::npos;
    caps_.nack = a.find("a=rtcp-fb:111 nack") != std::string::npos;
  }
  try {
    pc_->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
  } catch (const std::exception &e) {
    if (err) *err = e.what();
    return false;
  }
  state_ = State::Connecting;
  LOGI("listener %d: answer applied (red=%d twcc=%d nack=%d)", id_, caps_.red, caps_.twcc, caps_.nack);
  return true;
}

void Listener::close(const std::string &reason) {
  if (closing_.exchange(true)) return;
  close_reason_ = reason;
  if (dc_open_) send_ctl(json{{"t", "bye"}, {"reason", reason}}.dump());
  state_ = State::Closed;
}

bool Listener::dead() const {
  if (state_ == State::Closed) return true;
  int64_t now = now_us();
  if (state_ == State::Connecting && now - created_us_ > 30000000) return true;  // never connected: free the slot
  if (disconnected_since_ && now - disconnected_since_ > 10000000) return true;   // ICE lost for 10 s
  if (state_ == State::Live && now - last_ctl_us_ > 12000000) return true;        // page gone quiet
  return false;
}

void Listener::push(std::shared_ptr<const SourceBlock> b) {
  if (state_ != State::Live) return;
  std::lock_guard lk(qmu_);
  queue_.push_back(std::move(b));
  // A listener whose encoder cannot keep up must never back up the capture
  // thread; 200 ms behind means something is badly wrong, so drop the oldest.
  while (queue_.size() > 20) queue_.pop_front();
  qcv_.notify_one();
}

void Listener::sender_loop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  for (;;) {
    std::shared_ptr<const SourceBlock> b;
    {
      std::unique_lock lk(qmu_);
      qcv_.wait_for(lk, std::chrono::milliseconds(20), [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      if (!queue_.empty()) { b = std::move(queue_.front()); queue_.pop_front(); }
    }
    if (b) process_block(*b);

    int64_t now = now_us();
    if (state_ != State::Live) continue;
    // RTCP sender report once a second: gives the receiver's reports an RTT.
    if (now - last_sr_us_ > 1000000) {
      last_sr_us_ = now;
      std::vector<uint8_t> sr;
      {
        std::lock_guard lk(mu_);
        sr = build_sr(ssrc_, ntp_now(), rtp_ts_, packets_, octets_);
      }
      send_packet(std::move(sr), false);
    }
    // Return channel: receiver reports and NACKs for the listener's microphone.
    if (now - last_nack_us_ > 20000) {
      last_nack_us_ = now;
      auto nacks = talk_.take_nacks(now, cc_.rtt_ms());
      if (!nacks.empty() && talk_.ssrc()) send_packet(build_nack(ssrc_, talk_.ssrc(), nacks), false);
    }
    if (now - last_rr_us_ > 1000000) {
      last_rr_us_ = now;
      ReportBlock rb;
      if (talk_.report(rb)) send_packet(build_rr(ssrc_, rb), false);
    }
    if (now - last_stat_us_ > 1000000) {
      last_stat_us_ = now;
      send_stats();
      if (++diag_ % 5 == 0) {
        std::lock_guard lk(mu_);
        const Plan &p = cc_.plan();
        LOGD("listener %d: target %.0fk acked %.0fk loss %.1f%% rtt %.0fms queue %.0fms [%s] -> opus %dk x%d red %dms %s | sim drop q=%llu l=%llu | retx %llu | rx buf %.0fms conceal %.2f%%",
             id_, cc_.target_bps() / 1000, cc_.acked_bps() / 1000, cc_.loss() * 100, cc_.rtt_ms(), cc_.queue_ms(),
             cc_.state(), p.opus_bps / 1000, p.red, p.frame_ms, p.stereo ? "stereo" : "mono",
             (unsigned long long)(sim_ ? sim_->dropped_queue() : 0), (unsigned long long)(sim_ ? sim_->dropped_loss() : 0),
             (unsigned long long)retransmits_, rx_buffer_ms_, rx_conceal_ * 100);
      }
    }
  }
}

void Listener::process_block(const SourceBlock &b) {
  MixPrefs p;
  { std::lock_guard lk(mu_); p = prefs_; }
  float gm = p.music_mute ? 0.f : db_to_gain(std::clamp(p.music_db, -40.f, 12.f));
  float gv = (p.voice_mute || !b.mic_on) ? 0.f : db_to_gain(std::clamp(p.voice_db, -40.f, 12.f));
  float mix[kBlock * 2];
  memcpy(mix, b.system.data(), sizeof mix);
  ducker_.process(mix, kBlock, p.duck && b.voice_active && gv > 0.f, env_.duck_db);
  // Gains ramp across the block, so moving a slider never clicks.
  for (int i = 0; i < kBlock; ++i) {
    float t = float(i) / kBlock;
    float m = g_music_ + (gm - g_music_) * t;
    float v = (g_voice_ + (gv - g_voice_) * t) * b.voice[size_t(i)];
    mix[2 * i] = mix[2 * i] * m + v;
    mix[2 * i + 1] = mix[2 * i + 1] * m + v;
  }
  g_music_ = gm;
  g_voice_ = gv;
  limiter_.process(mix, kBlock);  // per listener: their own balance can never clip
  pcm_.insert(pcm_.end(), mix, mix + kBlock * 2);

  int frame_samples = kRate / 1000 * (applied_.frame_ms ? applied_.frame_ms : 20);
  while (int(pcm_.size()) >= frame_samples * 2) encode_frame();
}

void Listener::apply_plan(const Plan &p) {
  if (!enc_) return;
  if (p.opus_bps != applied_.opus_bps) opus_encoder_ctl(enc_, OPUS_SET_BITRATE(p.opus_bps));
  // Folding to mono is done inside the one stereo encoder, so it is seamless:
  // no new encoder, no decoder reset at the far end.
  if (p.stereo != applied_.stereo || !applied_.opus_bps)
    opus_encoder_ctl(enc_, OPUS_SET_FORCE_CHANNELS(p.stereo ? OPUS_AUTO : 1));
  if (p.fec != applied_.fec || !applied_.opus_bps) opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(p.fec ? 1 : 0));
  if (p.loss_pct != applied_.loss_pct || !applied_.opus_bps) opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(p.loss_pct));
  int complexity = env_.listener_count && *env_.listener_count > 6 ? 8 : 10;
  opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(complexity));
  applied_ = p;
}

void Listener::encode_frame() {
  Plan plan;
  PeerCaps caps;
  {
    std::lock_guard lk(mu_);
    caps_.max_bps = std::min(prefs_.max_kbps, env_.host_max_kbps ? env_.host_max_kbps->load() : 256) * 1000;
    caps = caps_;
    plan = cc_.update(caps, now_us());
  }
  // Frame duration only changes on a frame boundary: exactly here.
  int frame_ms = applied_.frame_ms ? applied_.frame_ms : 20;
  if (plan.opus_bps != applied_.opus_bps || plan.stereo != applied_.stereo || plan.fec != applied_.fec ||
      plan.loss_pct != applied_.loss_pct || plan.frame_ms != applied_.frame_ms || plan.red != applied_.red) {
    apply_plan(plan);
  }
  int n = kRate / 1000 * frame_ms;
  if (int(pcm_.size()) < n * 2) return;
  uint8_t out[1500];
  opus_int32 len = enc_ ? opus_encode_float(enc_, pcm_.data(), n, out, sizeof out) : -1;
  pcm_.erase(pcm_.begin(), pcm_.begin() + n * 2);
  uint32_t ts = rtp_ts_;
  rtp_ts_ += uint32_t(n);
  if (len <= 0) return;

  Frame primary{ts, std::vector<uint8_t>(out, out + len)};
  std::vector<uint8_t> pkt;
  {
    std::lock_guard lk(mu_);
    int red = caps_.red ? applied_.red : 0;
    std::vector<const Frame *> redundant;
    for (int k = red; k >= 1; --k)
      if (int(recent_.size()) >= k) redundant.push_back(&recent_[recent_.size() - size_t(k)]);
    uint16_t seq = seq_++;
    uint16_t tseq = twcc_seq_++;
    if (!redundant.empty()) {
      auto payload = build_red(redundant, primary, kPtOpus);
      pkt = build_rtp(kPtRed, seq, ts, ssrc_, first_packet_, tseq, payload.data(), payload.size());
    } else {
      pkt = build_rtp(kPtOpus, seq, ts, ssrc_, first_packet_, tseq, primary.data.data(), primary.data.size());
    }
    first_packet_ = false;
    recent_.push_back(std::move(primary));
    while (recent_.size() > 3) recent_.pop_front();
    Hist &h = history_[seq % kHistory];
    h.seq = seq;
    h.sent_us = now_us();
    h.resent_us = 0;
    h.pkt = pkt;
    cc_.on_sent(tseq, h.sent_us, int(pkt.size()) + 38);
    ++packets_;
    octets_ += uint32_t(len);
  }
  send_packet(std::move(pkt), true);
}

void Listener::send_packet(std::vector<uint8_t> &&pkt, bool media) {
  if (!track_ || !track_->isOpen()) return;
  {
    std::lock_guard lk(mu_);
    int64_t now = now_us();
    wire_log_.push_back({now, int(pkt.size()) + 38});
    while (!wire_log_.empty() && now - wire_log_.front().first > 2000000) wire_log_.pop_front();
  }
  (void)media;
  if (sim_) { sim_->send(std::move(pkt)); return; }  // RTCP too, so RTT includes the simulated delay
  try {
    track_->send(reinterpret_cast<const std::byte *>(pkt.data()), pkt.size());
  } catch (const std::exception &e) {
    LOGD("listener %d send: %s", id_, e.what());
  }
}

void Listener::on_rtp_or_rtcp(const rtc::binary &msg) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(msg.data());
  size_t n = msg.size();
  int64_t now = now_us();
  if (is_rtcp(p, n)) {
    RtcpEvents ev;
    parse_rtcp(p, n, ssrc_, ev);
    std::vector<std::vector<uint8_t>> resend;
    {
      std::lock_guard lk(mu_);
      for (auto &rb : ev.reports) {
        if (rb.lsr) {
          uint32_t mid = uint32_t(ntp_now() >> 16);
          double rtt = double(uint32_t(mid - rb.lsr - rb.dlsr)) * 1000.0 / 65536.0;
          if (rtt >= 0 && rtt < 5000) cc_.on_rtt(rtt, now);
        }
        cc_.on_rr_loss(rb.fraction_lost / 256.0, now);
      }
      if (!ev.twcc.empty()) cc_.on_twcc(ev.twcc, now);
      if (ev.remb_bps) cc_.on_remb(*ev.remb_bps);
      // Retransmit what the receiver can still use, within a budget of ~30 %
      // of the target so repair can never itself congest the link.
      double dt = last_budget_us_ ? double(now - last_budget_us_) / 1e6 : 0;
      last_budget_us_ = now;
      resend_budget_ = std::min(resend_budget_ + dt * cc_.target_bps() * 0.3 / 8.0, 16000.0);
      double rtt = std::max(cc_.rtt_ms(), 20.0);
      // Chrome sends NACKs for audio even when its SDP does not say so:
      // trust what arrives over what was negotiated.
      if (!ev.nacks.empty()) caps_.nack = true;
      for (uint16_t s : ev.nacks) {
        Hist &h = history_[s % kHistory];
        if (h.seq != s || h.pkt.empty()) continue;
        if (now - h.sent_us > 1500000) continue;                           // too old to matter
        if (h.resent_us && double(now - h.resent_us) / 1000.0 < rtt) continue;  // already on its way
        if (resend_budget_ < double(h.pkt.size())) break;
        resend_budget_ -= double(h.pkt.size());
        h.resent_us = now;
        std::vector<uint8_t> copy = h.pkt;
        uint16_t tseq = twcc_seq_++;
        set_twcc_seq(copy, tseq);
        cc_.on_sent(tseq, now, int(copy.size()) + 38, true);
        resend.push_back(std::move(copy));
        ++retransmits_;
      }
    }
    if (ev.has_sr) talk_.on_sender_report(ev.sr_ntp, now);
    for (auto &r : resend) send_packet(std::move(r), true);
    return;
  }
  RtpPacketInfo rtp;
  if (!parse_rtp(p, n, rtp)) return;
  if (!env_.talk_allowed || !*env_.talk_allowed) return;
  if (rtp.pt == kPtOpus || rtp.pt == kPtRed) talk_.push(rtp, now);
}

void Listener::mix_talk(float *out, int n, float gain) {
  if (state_ != State::Live) return;
  talk_.mix_into(out, n, gain);
}

void Listener::on_control(const std::string &text) {
  json m = json::parse(text, nullptr, false);
  if (m.is_discarded() || !m.is_object()) return;
  std::string t = m.value("t", "");
  if (t == "rx") {
    std::lock_guard lk(mu_);
    rx_buffer_ms_ = m.value("buf", 0.0);
    rx_conceal_ = m.value("conceal", 0.0);
    rx_rtt_ms_ = m.value("rtt", 0.0);
    rx_path_ = m.value("path", "");
    cc_.on_receiver(rx_buffer_ms_, rx_conceal_);
    if (rx_rtt_ms_ > 0 && cc_.rtt_ms() == 0) cc_.on_rtt(rx_rtt_ms_, now_us());
  } else if (t == "prefs") {
    std::lock_guard lk(mu_);
    prefs_.music_db = std::clamp(m.value("music_db", prefs_.music_db), -40.f, 12.f);
    prefs_.voice_db = std::clamp(m.value("voice_db", prefs_.voice_db), -40.f, 12.f);
    prefs_.music_mute = m.value("music_mute", prefs_.music_mute);
    prefs_.voice_mute = m.value("voice_mute", prefs_.voice_mute);
    prefs_.duck = m.value("duck", prefs_.duck);
    prefs_.max_kbps = std::clamp(m.value("max_kbps", prefs_.max_kbps), 16, 320);
  } else if (t == "name") {
    std::string nm = m.value("name", "");
    if (nm.size() > 40) nm.resize(40);
    std::lock_guard lk(mu_);
    name_ = nm;
  } else if (t == "ping") {
    send_ctl(json{{"t", "pong"}, {"id", m.value("id", 0)}}.dump());
  } else if (t == "bye") {
    LOGI("listener %d left", id_);
    state_ = State::Closed;
  }
}

void Listener::send_ctl(const std::string &s) {
  auto dc = dc_;
  if (!dc || !dc->isOpen()) return;
  try { dc->send(s); } catch (...) {}
}

void Listener::notify_settings() {
  json m = {{"t", "hello"}, {"v", 1}, {"name", env_.host_name},
            {"talk", env_.talk_allowed && env_.talk_allowed->load()},
            {"mic", env_.mic_on && env_.mic_on->load()},
            {"listeners", env_.listener_count ? env_.listener_count->load() : 1}};
  send_ctl(m.dump());
}

void Listener::send_stats() {
  ListenerView v = view();
  json m = {{"t", "stat"},       {"opus", int(v.opus_kbps)}, {"wire", int(v.wire_kbps)},
            {"target", int(v.target_kbps)}, {"frame", v.frame_ms}, {"red", v.red},
            {"stereo", v.stereo}, {"fec", v.fec}, {"loss", v.loss}, {"rtt", int(v.rtt_ms)},
            {"queue", int(v.queue_ms)}, {"cc", v.cc_state}, {"why", v.why},
            {"retx", v.retransmits}, {"mic", env_.mic_on && env_.mic_on->load()},
            {"speaking", env_.mic_active && env_.mic_active->load()},
            {"talk", env_.talk_allowed && env_.talk_allowed->load()},
            {"twcc", v.twcc}, {"nack", v.nack}, {"redcap", v.redcap}};
  send_ctl(m.dump());
}

ListenerView Listener::view() const {
  ListenerView v;
  v.id = id_;
  State s = state_;
  v.state = s == State::Live ? "live" : s == State::Connecting ? "connecting" : s == State::Offering ? "offering" : "closed";
  {
    std::lock_guard lk(mu_);
    const Plan &p = cc_.plan();
    v.name = name_;
    v.path = rx_path_;
    v.why = p.why;
    v.cc_state = cc_.state();
    v.rtt_ms = cc_.rtt_ms() > 0 ? cc_.rtt_ms() : rx_rtt_ms_;
    v.loss = cc_.loss();
    v.queue_ms = cc_.queue_ms();
    v.target_kbps = cc_.target_bps() / 1000.0;
    v.opus_kbps = applied_.opus_bps / 1000.0;
    v.frame_ms = applied_.frame_ms;
    v.red = caps_.red ? applied_.red : 0;
    v.stereo = applied_.stereo;
    v.fec = applied_.fec;
    v.twcc = caps_.twcc; v.nack = caps_.nack; v.redcap = caps_.red;
    v.rx_buffer_ms = rx_buffer_ms_;
    v.rx_conceal = rx_conceal_;
    v.retransmits = retransmits_;
    v.packets = packets_;
    v.prefs = prefs_;
    if (wire_log_.size() > 1) {
      double bytes = 0;
      for (auto &w : wire_log_) bytes += w.second;
      double span = std::max(double(wire_log_.back().first - wire_log_.front().first) / 1e6, 0.5);
      v.wire_kbps = bytes * 8 / span / 1000.0;
    }
  }
  auto ts = talk_.stats();
  v.talking = talk_.active() && ts.playing;
  v.talk_level_db = ts.level_db;
  v.connected_s = live_since_ ? (now_us() - live_since_) / 1e6 : 0;
  // Bars: loss after repair and RTT, the two things a listener actually hears.
  double bad = v.rx_conceal * 100 + v.loss * 20 + v.rtt_ms / 150.0;
  v.quality = s != State::Live ? 0 : bad < 0.5 ? 4 : bad < 1.5 ? 3 : bad < 4 ? 2 : 1;
  return v;
}

}  // namespace aoi
