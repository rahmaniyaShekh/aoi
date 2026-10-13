# mac: the macOS host (not started)

AOI ships for Windows only. The Windows host lives in `host/` and builds
nothing from this folder. A macOS host will be its own project here, with its
own build, so Windows and macOS code never mix.

What is here is an early sketch kept for reference: stubs of the interfaces
the Windows host's core uses (`host/src/platform/*.h`). Everything that needs
real OS work returns "not supported on macOS yet"; the POSIX parts
(`os_mac.cpp`: clocks, paths, randomness, processes, console) work. It is not
compiled anywhere.

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
