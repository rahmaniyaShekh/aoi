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
| `aoi start --split` | Split mode: your voice in the left channel, this PC's sound in the right, never mixed on the way. Each listener sets both levels and keeps them apart (voice left ear, sound right ear) or mixes them. `aoi restart --split` switches a running host. |
| `aoi status [-w]` | Listeners and live link numbers (`-w` refreshes every second, `--json` for scripts). |
| `aoi code` | Shows the code, link and QR again. |
| `aoi stop` / `aoi restart` | Graceful stop (listeners are told, the code is withdrawn) in about 1 s. Forced after 6 s if ever needed. |
| `aoi logs [-f]` | The log (`%LOCALAPPDATA%\AOI\aoi.log`). |
| `aoi new-code` | New code; everyone on the old code is disconnected. |
| `aoi kick <n>` | Removes listener #n. |
| `aoi mic [list\|<n>\|<name>\|laptop\|auto\|off]` | Which microphone. Automatic by default (see below); a choice is remembered. |
| `aoi speaker [list\|<n>\|<name>\|laptop\|auto]` | Where listeners' voices play. Automatic by default. |
| `aoi run` | Runs in the terminal instead of the background; Ctrl+C stops. |
| `aoi install` / `aoi uninstall [--purge]` | Per-user install: `%LOCALAPPDATA%\Programs\AOI`, user PATH, Apps & features entry. Never starts at sign-in. |
| `aoi selftest` | 41 checks on this PC: crypto, packet formats, DSP, voice processing, device choice, jitter buffer, congestion control, live devices. |
| `aoi echotest` | Plays a voice and music through this PC's speakers for 35 s and reports how much echo the mic would send to listeners. Stop AOI first. |

There are no settings. Defaults are fixed for the best result: system audio
plus microphone (if there is one), echo cancellation whenever sound plays on
speakers, talkback on, auto level on, adaptive Opus up to 256 kbps stereo, full loss protection, at most 2
listeners. Each listener sets their own balance on their page.

## No echo: the host's voice chain

A laptop's microphone hears its own speakers: the music, and the listeners'
voices coming back (talkback). Sent on as they are, listeners hear themselves
a moment later and the music twice. AOI removes it on the host, per 10 ms,
with WebRTC's audio processing (the same AEC3 echo canceller, noise
suppression and gain control Chrome uses):

- **Only when there is echo.** The canceller runs while sound plays on
  speakers the mic can hear. With earphones, earbuds or a headset there is
  no echo, so it switches itself off (and back on if the sound moves to the
  speakers): a canceller with nothing to cancel can only take the owner's
  voice away. `aoi status` shows which.
- **A laptop or desk microphone is opened raw** when the device allows it
  (most do): no driver AGC, noise gate or add-on suite (Nahimic, Waves,
  DTS...). Those change their gain all the time, which no echo canceller can
  follow; AOI does the same jobs itself. Headset and Bluetooth mics stay in
  Windows' normal mode (raw skips their gain: a Bluetooth headset's raw
  speech measured -59 dBFS).
- **The echo reference is what the speakers actually play**: the output
  device's loopback, after every app, our talkback and the device's own
  effects are mixed. One signal, one delay to the mic.
- **The mic is held back 40 ms** inside the canceller. Windows hands over the
  loopback only after mixing a period, and a raw laptop mic can deliver the
  echo of that period sooner; the canceller must see the sound before its
  echo.
- **Voice activity** (for the "talking" light) comes from an RNN voice
  detector on the cleaned signal, with a level floor: music, keys, a fan or
  the faint remains of a listener's voice cannot trigger it.

Measured on a Realtek + Intel Smart Sound laptop with Nahimic, speakers at
normal volume (`aoi echotest`): echo reduced by 31-46 dB, leaving it at
-50 to -65 dBFS, below the room's own noise. Before (1.0.3: no canceller,
level-based voice detection) music from the speakers counted as "talking": the
clean music was ducked 8 dB while a boosted, room-delayed copy of it, and of
the listener's own voice, went out through the mic.

If Windows' input level for the mic is below 50% (7% was seen), AOI raises
it to 100% when it opens the mic: otherwise the auto level has to add 30 dB
or more, and the noise and every Bluetooth glitch with it.

The listener's voice is cleaned up on the host as well (noise suppression and
auto level, per listener, about 4 dB above the host's own voice level) so it
stays clear over the music playing on the same output.

Automatic device choice: a USB or wired headset mic first, then a Bluetooth LE
Audio headset, then Windows' default mic. A classic Bluetooth headset mic is
used only when chosen (`aoi mic buds`) or when it is the only one, because
opening it switches the headset to call quality. Listeners' voices play on the
same headset as the mic, otherwise on Windows' default output.

## The listener page

- **Sound | Mic | Leave**, with the mic in the middle. Turning the mic **on**
  asks for confirmation first; turning it off is immediate.
- **Music** (the host's speaker output) and **Voice** (the host's microphone)
  sliders, each with mute. The host applies them to that listener's own mix
  (see below), so they cost no bandwidth and cannot put voice and music out
  of sync. Nothing is ever lowered automatically: no ducking.
- In **split mode** the same sliders work on the page itself, plus *Keep them
  apart* (on: their voice in the left ear, their PC's sound in the right;
  off: both in both ears). The PC's sound is mono in this mode, as its
  channel carries one side each.
- **I'm using earphones** (on by default). The listener's mic opens raw: no
  echo cancellation, noise suppression or auto gain in the browser. Asking a
  phone for any of them puts it in "call mode", where its own voice
  processing cancels or ducks the music for as long as the mic is on. The
  host cleans the voice up instead (noise suppression and level, per
  listener). A Bluetooth earbud mic is avoided when the device has another
  (it drops the earbuds to phone-call quality). Turned off for listening on a
  speaker: the browser's full processing, so the host hears no echo.
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
| Jitter vs congestion | n/a | Delay spikes that come and go (Wi-Fi retries, mobile scheduling) are not a queue: the rate is cut only when the minimum delay itself stays up, or packets are lost. (1.0.4 and earlier cut on jitter alone and could sink a fast but jittery link to ~12-50 kbps.) |
| Listener buffer | Adaptive ring | Browser NetEq with an adaptive target: grows the moment audio is concealed, shrinks after clean stretches, and always leaves room (1.5×RTT) for a retransmission to arrive in time |
| Limiter | 0.97 ceiling, one-block look-ahead | True sliding-window look-ahead limiter, −1 dBFS ceiling: a lossy decoder can overshoot its input |
| Level | RMS makeup gain | ITU-R BS.1770 K-weighted loudness, gated, 70th percentile, bounded slew |
| Mix | Sender-side, one balance for everyone | Per listener: each chooses music/voice levels and ducking; one encoder per listener on one clock |

Kept from ASOW because they were right: process-loopback capture excluding our
own process tree (no driver compressor in the path, Bluetooth-proof, and
talkback never sent back to listeners through the capture), ducking, and
drift-correcting fractional readers for the second clock (mic, talkback). The
mic chain is now WebRTC's (see above).

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

## Diagnostics

Every 10 s the log (`aoi logs`) has one line per listener: path, bitrate,
round trip, loss, their buffer, how much of their audio had to be filled in,
the relay queue, and whether their mic is on and how loud. When a listener
joins it logs their browser and device; when they turn their mic on, how the
browser opened it (echo cancellation, noise suppression, auto gain).

## Using both apps together

The screen-share app (SOI) keeps `share.mdarif.online` as its Custom Domain; AOI
is a separate Worker on the route `share.mdarif.online/aoi*`, which runs in front
of it for that path only. Separate Durable Objects, separate codes, separate
browser storage keys (`aoi.*` vs `soi.*`), separate instance locks. Both hosts
can run on the same PC at once.

## When two networks cannot reach each other: the relay

Most listeners connect directly, peer to peer. Two kinds of networks cannot:
ones that block UDP altogether (schools, offices, some mobile carriers), and
pairs of NATs that will not talk to each other (a carrier-grade NAT on one side
and a strict router on the other). The page switches to the **relay** by
itself:

- UDP blocked: a STUN probe, run alongside the first attempt, gets no answer.
  The page goes straight to the relay: listening within about 1-5 s.
- No path between two NATs: connectivity checks get no reply at all for 5 s.
- Once a tab has needed the relay for a host, it uses it first next time.
- The host connects to the relay in several short attempts (3 s each): some
  networks silently drop the first connection to one Cloudflare address.

How the relay works:

- The page puts a fresh AES-256 key inside its code-sealed answer. The host
  opens it, and both connect a WebSocket to the AOI Worker, which pairs them by
  the handshake session and forwards the messages. Every message is AES-GCM
  encrypted end to end; the server relays ciphertext it cannot read.
- The host sends Opus in ~80 ms batches (40 ms frames, no redundancy: the
  path is TCP, nothing is lost). Sending has its own thread and queue, so a
  slow link never stalls the encoder: more than 0.4 s of waiting audio is
  dropped (late audio is useless) and the bitrate falls at once. It adapts
  16-192 kbps to the page's underruns, the send queue and send times,
  starting at 64 kbps and climbing by a quarter every 5 clean seconds. The page decodes with the
  browser's built-in Opus decoder and plays through a small adaptive jitter
  buffer (`relay-worklet.js`). Talkback works the same way in reverse.
- It runs on the **free Workers plan**: about 12 messages a second per relayed
  listener, billed at 1/20 of a request, so roughly 20 hours of relayed
  listening a day. Direct connections never touch it.
- Needs host **1.0.2** or later (`aoi update`); older hosts keep working
  direct-only, and the page tells the listener to ask for the update.

`aoi status` shows a relayed listener with `relay` as its path.

Optional: if a Cloudflare Realtime TURN key is ever added (`wrangler secret put
TURN_KEY_ID` / `TURN_KEY_TOKEN` in `rendezvous/`), browsers also get UDP relay
candidates and many of these pairs connect through TURN instead.

## Layout

```
host/                 the Windows host (C++20, one static exe, no runtime dependencies)
  src/core/           platform-neutral algorithms; no OS calls (checked on every build)
    engine.*          capture -> condition -> fan out; devices followed live
    voice.*           WebRTC APM: AEC3 echo canceller, noise suppression, AGC2, voice detector
    devices.*         which mic / talkback output, and why
    dsp.*             loudness, auto level, look-ahead limiter, drift reader
    listener.*        one peer: mix, Opus encoder, RTP/RED/NACK, control channel, relay
    cc.*              congestion control + encoding plan
    rtp.*             RTP/RTCP/RED/NACK/TWCC by hand
    talkback.*        the return channel's jitter buffer (FEC, PLC, drift servo)
    rendezvous.*      signalling client (hibernating WebSocket, HTTP fallback)
    crypto.*          codes + sealed blob (AES-256-GCM, PBKDF2, raw deflate)
    netsim.*          bottleneck/loss/jitter simulator for testing
    cli.* control.*   commands, detached daemon, control channel
    selftest*.cpp echotest.cpp
  src/platform/*.h    the interfaces core uses to reach the OS
  src/platform/win/   WASAPI (process loopback, raw mic, loopback reference), WinHTTP, named pipes, installer
mac/                  the macOS host: a separate project (not started; nothing here is built)
rendezvous/           Cloudflare Worker + Durable Object + listener page
  public/aoi/         the listener page + relay-worklet.js (the installer is a GitHub release, not served here)
  test/               contract tests (run against local or production)
install.ps1           installer: fetches aoi.exe from the latest release with gh
third_party/          dependency sources, fetched by host/deps.ps1 (not committed)
```

## Building and deploying

```powershell
# once: MSYS2 at C:\msys64 with mingw-w64-x86_64-gcc, -openssl and -zlib, and Python 3
powershell -ExecutionPolicy Bypass -File host\deps.ps1       # pinned opus, libdatachannel, speexdsp, qrcodegen, webrtc-audio-processing
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
