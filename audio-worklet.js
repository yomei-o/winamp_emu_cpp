// Drains the PCM ring the emulator fills (main_wasm.cpp has the layout):
// 16-bit stereo frames at the guest's sample rate, resampled linearly to the
// AudioContext's.  The frames-played counter it advances is what tells the
// guest's waveOut that a buffer has finished, so Winamp runs at the speed the
// sound actually goes out.
class WaemuOut extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.h = new Int32Array(options.processorOptions.header);
    this.pcm = new Int16Array(options.processorOptions.pcm);
    this.cap = (this.pcm.length / 2) | 0;
    this.frac = 0;
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
    const h = this.h, pcm = this.pcm, cap = this.cap;
    let read = Atomics.load(h, 1) >>> 0;
    const written = Atomics.load(h, 0) >>> 0;
    const drop = Atomics.load(h, 6) >>> 0;
    if (((drop - read) | 0) > 0 && ((written - drop) | 0) >= 0) read = drop;  // waveOutReset
    const rate = Atomics.load(h, 3) || sampleRate;
    const step = rate / sampleRate;
    const vl = (Atomics.load(h, 4) >>> 0) / 65535 / 32768;
    const vr = (Atomics.load(h, 5) >>> 0) / 65535 / 32768;
    if (Atomics.load(h, 2)) {  // paused
      L.fill(0); if (R !== L) R.fill(0);
      Atomics.store(h, 1, read | 0);
      return true;
    }
    let frac = this.frac;
    for (let i = 0; i < n; i++) {
      const avail = (written - read) | 0;
      if (avail < 2) {  // underrun: silence, and do not advance
        L[i] = 0; if (R !== L) R[i] = 0;
        continue;
      }
      const a = (read % cap) * 2, b = ((read + 1) % cap) * 2;
      L[i] = (pcm[a] + (pcm[b] - pcm[a]) * frac) * vl;
      if (R !== L) R[i] = (pcm[a + 1] + (pcm[b + 1] - pcm[a + 1]) * frac) * vr;
      frac += step;
      const whole = Math.floor(frac);
      frac -= whole;
      read = (read + whole) >>> 0;
    }
    this.frac = frac;
    Atomics.store(h, 1, read | 0);
    return true;
  }
}
registerProcessor('waemu-out', WaemuOut);
