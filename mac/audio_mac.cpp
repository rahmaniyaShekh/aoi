// platform/audio_system.h on macOS: not supported yet. See README.md for
// what a real port uses (ScreenCaptureKit for system audio, AVAudioEngine
// voice processing or AUHAL for the mic, Core Audio for devices).
#include "platform/audio_system.h"

namespace aoi {

namespace {

class Unsupported final : public AudioStream {
 public:
  StreamStatus status() const override {
    StreamStatus s;
    s.error = "not supported on macOS yet";
    return s;
  }
};

class UnsupportedCapture final : public SystemCaptureStream {
 public:
  StreamStatus status() const override { return Unsupported().status(); }
  std::string source() const override { return "not supported on macOS yet"; }
  bool process_loopback() const override { return false; }
  uint64_t blocks() const override { return 0; }
};

class MacAudioStub final : public IAudioSystem {
 public:
  bool supported() const override { return false; }
  std::string backend() const override { return "none (macOS port not written)"; }
  std::vector<DeviceInfo> list_devices(DeviceKind) override { return {}; }
  std::unique_ptr<SystemCaptureStream> open_system_capture(StereoBlock) override {
    return std::make_unique<UnsupportedCapture>();
  }
  std::unique_ptr<AudioStream> open_mic(const std::string &, MonoSink) override { return std::make_unique<Unsupported>(); }
  std::unique_ptr<AudioStream> open_render(const std::string &, MonoPull) override { return std::make_unique<Unsupported>(); }
  std::unique_ptr<AudioStream> open_render_loopback(const std::string &, MonoSink) override {
    return std::make_unique<Unsupported>();
  }
  int subscribe(EventSink) override { return 0; }
  void unsubscribe(int) override {}
  std::optional<float> input_volume(const std::string &) override { return std::nullopt; }
};

}  // namespace

std::unique_ptr<IAudioSystem> create_audio_system() { return std::make_unique<MacAudioStub>(); }

}  // namespace aoi
