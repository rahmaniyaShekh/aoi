# AOI: Audio Over Internet

Share everything a Windows PC plays, plus its microphone, with up to two
listeners anywhere. They open a link in any browser; nothing to install on
their side. This is ASOW (the LAN version) rebuilt for the internet: WebRTC,
peer to peer, designed to stay unbroken on slow and lossy connections.

```powershell
# install + start (no admin). The repository is private: this uses the GitHub
# CLI, logged in (`gh auth login`) to an account with access.
gh release download -R rahmaniyaShekh/aoi -p install.ps1 -O - | Out-String | iex

aoi start      # start in the background: prints code, link and a QR code
aoi status     # who is listening, bitrate, RTT, loss, buffer, protection
aoi stop       # stop (or: taskkill /im aoi.exe)
```

Listeners go to **https://share.mdarif.online/aoi** and type the code, or open
`https://share.mdarif.online/aoi/K7Q-4MX` directly.

## Commands

| Command | What it does |
|---|---|
| `aoi start` | Starts detached, waits until the code is live, prints code, link and QR. Running it again just shows the code. |
| `aoi status [-w]` | Listeners and live link numbers (`-w` refreshes every second, `--json` for scripts). |
| `aoi code` | Shows the code, link and QR again. |
| `aoi stop` / `aoi restart` | Graceful stop (listeners are told, the code is withdrawn) in about 1 s. Forced after 6 s if ever needed. |
| `aoi logs [-f]` | The log (`%LOCALAPPDATA%\AOI\aoi.log`). |
| `aoi new-code` | New code; everyone on the old code is disconnected. |
| `aoi kick <n>` | Removes listener #n. |
| `aoi run` | Runs in the terminal instead of the background; Ctrl+C stops. |
| `aoi install` / `aoi uninstall [--purge]` | Per-user install: `%LOCALAPPDATA%\Programs\AOI`, user PATH, Apps & features entry. Never starts at sign-in. |
| `aoi selftest` | 27 checks on this PC: crypto, packet formats, DSP, jitter buffer, congestion control, live devices. |

There are no settings. Defaults are fixed for the best result: system audio
plus microphone (if there is one), talkback on, auto level on, adaptive
Opus up to 256 kbps stereo, full loss protection, at most 2 listeners. Each
listener sets their own balance on their page.

## The listener page

- **Sound | Mic | Leave**, with the mic in the middle. Turning the mic **on**
  asks for confirmation first; turning it off is immediate.
- **Music** (the host's speaker output) and **Voice** (the host's microphone)
  sliders, each with mute, plus *Lower music while they talk*. The host
  applies them to that listener's own mix (see below), so they cost no
  bandwidth and cannot put voice and music out of sync.
- **Low delay / Balanced / Unbreakable** buffering, and **Data saver** (~48 kbps).
- Remembers hosts that worked (rename or forget), deep links, reconnects on its
  own when the host restarts, waits for a free spot when two people are already
  listening, keeps the screen awake, lock-screen media controls.

## What was improved over ASOW, and why

ASOW was tuned for one campus Wi-Fi: lossless PCM/ALS over TCP/WebSocket, a
128 kbps floor, a controller that watched its own send queue, and receivers that
concealed dropped packets. Over the internet each of those becomes a weakness.

| | ASOW (LAN) | AOI (internet) |
|---|---|---|
| Transport | TCP/WebSocket: one lost packet stalls everything behind it | RTP over UDP (WebRTC, DTLS-SRTP): loss costs only that packet |
| Codec | PCM / ALS lossless, floor ~128 kbps | Opus, transparent at 256 kbps stereo, usable down to ~10 kbps |
| Loss repair | Receiver pitch-period concealment only | RED redundancy (up to 3 older frames per packet), NACK retransmission, Opus in-band FEC, then the browser's NetEq concealment |
| Congestion signal | Own send-queue backlog (right for TCP on a LAN) | Per-packet arrival times (transport-wide feedback) → trendline delay detector, plus a jitter-proof queue estimate (1 s vs 10 s minimum one-way delay), plus loss |
| Shape of the stream | Discrete rungs | Continuous: bitrate, 20/40/60 ms frames (header overhead matters at 24 kbps), stereo→mono in the same encoder (seamless), redundancy depth with hysteresis |
| Random loss vs congestion | Treated alike | Loss with no queue building (Wi-Fi, mobile) is answered with protection, not a rate cut; loss is judged on original packets only, since retransmissions die in the same burst |
| Listener buffer | Adaptive ring | Browser NetEq with an adaptive target: grows the moment audio is concealed, shrinks after clean stretches, and always leaves room (1.5×RTT) for a retransmission to arrive in time |
| Limiter | 0.97 ceiling, one-block look-ahead | True sliding-window look-ahead limiter, −1 dBFS ceiling: a lossy decoder can overshoot its input |
| Level | RMS makeup gain | ITU-R BS.1770 K-weighted loudness, gated, 70th percentile, bounded slew |
| Mix | Sender-side, one balance for everyone | Per listener: each chooses music/voice levels and ducking; one encoder per listener on one clock |

Kept from ASOW because they were right: process-loopback capture excluding our
own process tree (no driver compressor in the path, Bluetooth-proof, and
talkback on one device without echo), the mic chain (high-pass, noise-floor gate,
speech-only AGC, ducking), and drift-correcting fractional readers for the
second clock (mic, talkback).

## Measured

On this machine (`aoi selftest`, and real Chrome against the built-in network
simulator `aoi start --simulate rate=..,loss=..,burst=..,delay=..,jitter=..`):

| Link | Result |
|---|---|
| Fast | 256 kbps stereo Opus, 0 % loss; stereo channel separation 36 dB end to end |
| 64 kbps, 3 % bursty loss, 160 ms added delay, 30 ms jitter | Uses the link fully, **0.00 % of audio concealed** at the listener over 60 s |
| 32 kbps, 10 % bursty loss, ~210 ms RTT, 60 ms jitter | Steady at 21–26 kbps, **0.00 % concealed** after the first seconds |
| Capacity collapses 600 → 48 kbps | Queue drained 1.8 s after the drop, then fits |
| 24 kbps (2G class) | 23 kbps used, 60 ms frames |
| Talkback, 10 % loss + 40 ms jitter | 969/969 blocks audible |
| Start → code live | ~0.9 s; stop ~1 s |

## Using both apps together

The screen-share app (SOI) keeps `share.mdarif.online` as its Custom Domain; AOI
is a separate Worker on the route `share.mdarif.online/aoi*`, which runs in front
of it for that path only. Separate Durable Objects, separate codes, separate
browser storage keys (`aoi.*` vs `soi.*`), separate instance locks. Both hosts
can run on the same PC at once.

## Worth doing once: TURN

Most listeners connect directly. When *both* sides sit behind strict NAT (some
mobile carriers, some offices), a relay is needed. The Worker already hands out
Cloudflare TURN credentials when two secrets exist:

1. Cloudflare dashboard → **Realtime → TURN Server** → create a TURN key.
2. In `rendezvous/`:
   ```bash
   npx wrangler secret put TURN_KEY_ID
   npx wrangler secret put TURN_KEY_TOKEN
   ```

Nothing else changes; `aoi status` then shows "relay available".

## Layout

```
host/                 the Windows host (C++20, one static exe, no runtime dependencies)
  src/audio.*         WASAPI: process loopback, endpoint fallback, mic, talkback output
  src/dsp.*           loudness, auto level, look-ahead limiter, voice chain, drift reader
  src/listener.*      one peer: mix, Opus encoder, RTP/RED/NACK, control channel
  src/cc.*            congestion control + encoding plan
  src/rtp.*           RTP/RTCP/RED/NACK/TWCC by hand
  src/talkback.*      the return channel's jitter buffer (FEC, PLC, drift servo)
  src/rendezvous.*    signalling client (hibernating WebSocket, HTTP fallback)
  src/crypto.*        codes + sealed blob (AES-256-GCM, PBKDF2, raw deflate)
  src/netsim.*        bottleneck/loss/jitter simulator for testing
  src/cli.* control.* commands, detached daemon, named-pipe control
rendezvous/           Cloudflare Worker + Durable Object + listener page
  public/aoi/         the listener page (the installer is a GitHub release, not served here)
  test/               contract tests (run against local or production)
install.ps1           installer: fetches aoi.exe from the latest release with gh
third_party/          dependency sources, fetched by host/deps.ps1 (not committed)
```

## Building and deploying

```powershell
# once: MSYS2 at C:\msys64 with mingw-w64-x86_64-gcc, -openssl and -zlib
powershell -ExecutionPolicy Bypass -File host\deps.ps1       # pinned opus, libdatachannel, speexdsp, qrcodegen
powershell -ExecutionPolicy Bypass -File host\build.ps1      # -> dist\aoi.exe
dist\aoi.exe selftest

# release the host: a private GitHub release (the website does not serve it)
gh release create v1.0.1 dist\aoi.exe install.ps1 -R rahmaniyaShekh/aoi --title "AOI 1.0.1" --notes "..."

# the rendezvous Worker and the listener page
cd rendezvous
npm install
npx wrangler deploy
node test\rendezvous.test.mjs https://share.mdarif.online/aoi
```
