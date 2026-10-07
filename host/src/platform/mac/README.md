# platform/mac: compile-gated stub

AOI ships for Windows only. This directory implements the same platform
interfaces (`../os.h`, `../audio_system.h`, `../http.h`, `../ipc.h`,
`../installer.h`) so the core builds on macOS (`if(APPLE)` in
`host/CMakeLists.txt`). Everything that needs real OS work returns
"not supported on macOS yet". The POSIX parts (`os_mac.cpp`: clocks, paths,
randomness, processes, console) already work.

A real port would use:

| Interface | macOS API |
|---|---|
| `open_system_capture` | **ScreenCaptureKit** audio (`SCStream` with `capturesAudio = true`, `excludesCurrentProcessAudio = true`; macOS 13+). It is the closest match to Windows process loopback: every app's audio, minus our own talkback, before the output device. The fallback for macOS 14.2+ is a Core Audio process tap (`AudioHardwareCreateProcessTap`). |
| `open_mic` | **AVAudioEngine** input node with `setVoiceProcessingEnabled(true)` (Apple's own echo canceller, noise suppression and AGC), or a raw `AUHAL` input unit when WebRTC APM does the processing, as on Windows. |
| `open_render` | `AVAudioEngine` output node or `AUHAL` output unit on the chosen `AudioDeviceID`. |
| `open_render_loopback` | Not needed with voice processing (Apple's AEC takes the reference itself). With APM, a process tap on our own output plus the system mix. |
| `list_devices` / `subscribe` | Core Audio `kAudioHardwarePropertyDevices`, `kAudioDevicePropertyTransportType` (Bluetooth, BluetoothLE, USB, BuiltIn) and `kAudioObjectPropertyListener` for add/remove/default changes. |
| `input_volume` | `kAudioDevicePropertyVolumeScalar` on the input scope. |
| `http.h` | `NSURLSession` (HTTP and `NSURLSessionWebSocketTask`). |
| `ipc.h` | A UNIX domain socket in `~/Library/Application Support/AOI`, mode 0600. |
| `installer.h` | Copy to `~/Applications/AOI` (or `/usr/local/bin` with a symlink); no autostart. |

Bluetooth on macOS behaves like Windows: opening a classic Bluetooth headset's
mic switches it to the hands-free profile, so the same one-line note applies.
