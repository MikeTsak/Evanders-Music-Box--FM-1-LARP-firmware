// SPDX-License-Identifier: GPL-3.0-only
// Evander's Music Box: the emulator's audio thread. The whole firmware core (emb.wasm, built from the same
// src/ as the FM-1) lives here: the page sends it the controls, it renders the sound and sends back the
// screen and the LEDs. One thread owns the core, as the FM-1's audio ISR and main loop share it.

class MusicBox extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { wasm, stored } = options.processorOptions;
    this.stored = stored || {};                     // slot -> Uint8Array (from the page's localStorage)
    const env = {
      js_store: (slot, p, n) => {
        const bytes = new Uint8Array(this.mem.buffer, p, n).slice();
        this.stored[slot] = bytes;
        this.port.postMessage({ type: "store", slot, bytes });
        return 0;
      },
      js_load: (slot, p, n) => {
        const d = this.stored[slot];
        if (!d || d.length !== n) return -1;
        new Uint8Array(this.mem.buffer, p, n).set(d);
        return 0;
      },
    };
    this.ex = new WebAssembly.Instance(new WebAssembly.Module(wasm), { env }).exports;
    this.mem = this.ex.memory;
    this.ex.wasm_init((Math.random() * 0xFFFFFFFF) >>> 0);
    this.blocks = 0;
    this.port.onmessage = (ev) => this.onMessage(ev.data);
  }

  onMessage(m) {
    const ex = this.ex;
    switch (m.type) {
      case "button": ex.wasm_button(m.id, m.down ? 1 : 0); break;
      case "key": ex.wasm_key(m.id, m.down ? 1 : 0, m.vel || 100); break;
      case "enc": ex.wasm_encoder(m.id, m.steps); break;
      case "master": ex.wasm_master(m.q12); break;
      case "midi": ex.wasm_midi(m.st, m.d1, m.d2); break;
      case "sysex": {
        const inp = new Uint8Array(this.mem.buffer, ex.wasm_sysex_in(), 1024);
        inp.set(m.bytes.subarray(0, 1024));
        const n = ex.wasm_sysex(m.bytes.length);
        const out = new Uint8Array(this.mem.buffer, ex.wasm_sysex_out(), n).slice();
        this.port.postMessage({ type: "sysex", id: m.id, bytes: out });
        break;
      }
    }
  }

  process(_inputs, outputs) {
    const ex = this.ex, out = outputs[0], n = out[0].length;
    const p = ex.wasm_render(n);
    const f = new Float32Array(this.mem.buffer, p, 256);
    out[0].set(f.subarray(0, n));
    if (out[1]) out[1].set(f.subarray(128, 128 + n));
    // the main loop: ~60 times a second the controls' timers, ~30 the screen and the LEDs
    if ((++this.blocks & 3) === 0) ex.wasm_tick(Math.floor(currentTime * 1000));
    if ((this.blocks & 7) === 0) {
      const leds = new Uint8Array(this.mem.buffer, ex.wasm_leds(), 41).slice();
      let fb = null;
      if (ex.wasm_draw()) fb = new Uint16Array(this.mem.buffer, ex.wasm_fb(), 240 * 240).slice();
      this.port.postMessage({ type: "frame", leds, fb, voices: ex.wasm_status(0), peak: ex.wasm_status(1) },
                            fb ? [fb.buffer] : []);
    }
    return true;
  }
}
registerProcessor("music-box", MusicBox);
