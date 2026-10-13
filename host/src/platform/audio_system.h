// The one audio interface the core uses. This host implements it with WASAPI
// (platform/win/audio_win.cpp).
//
// Every stream delivers or pulls 48 kHz float; the implementation converts
// from whatever the device runs at (any rate, mono to multichannel, integer
// or float) and absorbs the device's own block sizes.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aoi {

enum class DeviceKind { Capture, Render };

enum class DeviceBus { Unknown, BuiltIn, Usb, Bluetooth, Hdmi, Virtual };

enum class FormFactor { Unknown, Speakers, LineLevel, Headphones, Microphone, Headset, Handset, Digital, Hdmi, Network };

struct DeviceInfo {
  std::string id;         // stable endpoint id (survives reboots and reconnects)
  std::string name;       // "Headset (CMF Buds 2a)"
  DeviceKind kind = DeviceKind::Capture;
  FormFactor form = FormFactor::Unknown;
  DeviceBus bus = DeviceBus::Unknown;
  std::string container;  // physical device: both endpoints of one headset share it
  bool builtin_container = false;  // the container is the PC itself
  bool default_console = false;    // Windows' default device
  bool default_comms = false;      // Windows' default communications device
  // Classic Bluetooth hands-free/headset profile: opening this mic switches
  // the headset to call quality for as long as it is open.
  bool bluetooth_hfp = false;
  bool bluetooth_le = false;  // LE Audio: stereo and mic can coexist
};

struct StreamStatus {
  bool running = false;
  std::string device_name;
  int rate = 0, channels = 0;  // the device's own format
  std::string format;          // "16-bit PCM", "32-bit float"
  double latency_ms = 0;       // device buffering, for the echo canceller's delay hint
  uint64_t frames = 0;         // delivered or pulled so far (48 kHz)
  std::string error;           // last failure, human readable; empty while healthy
  bool access_denied = false;  // the OS privacy switch blocks the microphone
  bool missing = false;        // the device is gone (unplugged, switched off)
};

class AudioStream {
 public:
  virtual ~AudioStream() = default;
  virtual StreamStatus status() const = 0;
};

// "What this PC is playing", minus our own process (talkback).
class SystemCaptureStream : public AudioStream {
 public:
  virtual std::string source() const = 0;
  virtual bool process_loopback() const = 0;
  virtual uint64_t blocks() const = 0;
};

enum class DeviceEvent { Added, Removed, StateChanged, DefaultChanged, PropertyChanged };

class IAudioSystem {
 public:
  // Exactly 480 frames (10 ms) of interleaved stereo per call.
  using StereoBlock = std::function<void(const float *stereo, int frames)>;
  // Mono, any number of frames per call.
  using MonoSink = std::function<void(const float *mono, int frames)>;
  using MonoPull = std::function<void(float *mono, int frames)>;
  using EventSink = std::function<void(DeviceEvent, const std::string &device_id)>;

  virtual ~IAudioSystem() = default;
  virtual bool supported() const = 0;
  virtual std::string backend() const = 0;  // "WASAPI"

  // Active endpoints, in a stable order (by name, then id).
  virtual std::vector<DeviceInfo> list_devices(DeviceKind kind) = 0;

  virtual std::unique_ptr<SystemCaptureStream> open_system_capture(StereoBlock cb) = 0;
  // A specific microphone. The stream keeps retrying on its own if the device
  // is briefly unavailable; status() says why it is not running.
  virtual std::unique_ptr<AudioStream> open_mic(const std::string &device_id, MonoSink cb) = 0;
  // Talkback output on a specific render device (mono, duplicated to all channels).
  virtual std::unique_ptr<AudioStream> open_render(const std::string &device_id, MonoPull pull) = 0;
  // Everything a render device actually plays, after its mix: the echo
  // canceller's reference. Silence is delivered while nothing plays.
  virtual std::unique_ptr<AudioStream> open_render_loopback(const std::string &device_id, MonoSink cb) = 0;

  // Device added/removed/state/default changes. Callbacks arrive on an OS
  // thread and must return quickly.
  virtual int subscribe(EventSink sink) = 0;
  virtual void unsubscribe(int token) = 0;

  // The endpoint's own input level (0..1) as set in the OS sound settings.
  virtual std::optional<float> input_volume(const std::string &device_id) = 0;
  // Sets it (0..1) and unmutes. False if the device has no such control.
  virtual bool set_input_volume(const std::string &device_id, float level) = 0;
};

std::unique_ptr<IAudioSystem> create_audio_system();

}  // namespace aoi
