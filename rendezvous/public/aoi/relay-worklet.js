// Audio worklets for the relay path (used only when the listener's network
// cannot reach the host directly and audio comes through the server).
//
// aoi-player: a stereo jitter buffer. Decoded audio arrives in bursts over a
// TCP path; this holds a target depth, starts once it is reached, and trims
// the playback rate by up to +-2 % to hold it there, so the host's clock and
// this sound card's clock can never drift the buffer empty or full.
// aoi-capture: forwards microphone samples to the page for talkback.

class AoiPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.inRate = o.inRate || 48000;
    this.step0 = this.inRate / sampleRate;  // input samples per output sample
    this.cap = this.inRate * 8;
    this.l = new Float32Array(this.cap);
    this.r = new Float32Array(this.cap);
    this.w = 0;          // samples written (absolute)
    this.rd = 0;         // fractional read position (absolute)
    this.target = (o.targetMs || 300) * this.inRate / 1000;
    this.playing = false;
    this.under = 0;      // output samples played as silence after a start
    this.played = 0;
    this.lastReport = 0;
    this.port.onmessage = e => {
      const m = e.data;
      if (m.type === 'pcm') this.write(m.l, m.r);
      else if (m.type === 'target') this.target = m.ms * this.inRate / 1000;
    };
  }
  write(l, r) {
    for (let i = 0; i < l.length; i++) {
      const k = (this.w + i) % this.cap;
      this.l[k] = l[i];
      this.r[k] = r ? r[i] : l[i];
    }
    this.w += l.length;
    if (this.w - this.rd > this.cap - this.inRate) this.rd = this.w - this.target;  // never overrun
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
    let fill = this.w - this.rd;
    if (!this.playing && fill >= this.target) this.playing = true;
    if (!this.playing) {
      L.fill(0); R.fill(0);
      if (this.played > 0) this.under += n;
    } else {
      // Far behind (a burst after a stall): jump to the target, don't play it all.
      if (fill > this.target * 2 + this.inRate) { this.rd = this.w - this.target; fill = this.target; }
      const err = (fill - this.target) / Math.max(this.target, 1);
      const step = this.step0 * (1 + Math.max(-0.02, Math.min(0.02, err * 0.02)));
      for (let i = 0; i < n; i++) {
        if (this.w - this.rd < 2) {
          // Ran dry: fade out what is left and wait for the buffer to refill.
          for (let j = i; j < n; j++) { L[j] = 0; R[j] = 0; }
          this.playing = false;
          this.under += n - i;
          break;
        }
        const k = Math.floor(this.rd), t = this.rd - k;
        const a = k % this.cap, b = (k + 1) % this.cap;
        L[i] = this.l[a] + (this.l[b] - this.l[a]) * t;
        R[i] = this.r[a] + (this.r[b] - this.r[a]) * t;
        this.rd += step;
        this.played++;
      }
    }
    if (currentTime - this.lastReport > 0.5) {
      this.lastReport = currentTime;
      this.port.postMessage({ fillMs: (this.w - this.rd) * 1000 / this.inRate, under: this.under,
        played: this.played, targetMs: this.target * 1000 / this.inRate, playing: this.playing });
    }
    return true;
  }
}

class AoiCapture extends AudioWorkletProcessor {
  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (ch && ch.length) this.port.postMessage(ch.slice(0));
    return true;
  }
}

registerProcessor('aoi-player', AoiPlayer);
registerProcessor('aoi-capture', AoiCapture);
