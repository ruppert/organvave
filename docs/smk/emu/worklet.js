// SPDX-License-Identifier: GPL-3.0-only
// The SMK-37 in the browser: the AudioWorklet (and its silent twin on the page's thread) that runs web/emu/felucca_web.c built
// with BOARD_SMK37. It takes the page's events (keys, pads, buttons, encoders, faders, wheels, MIDI) and hands back the screen,
// the button LEDs, the pads' colours and the saved flash. The FM-1's protocol is ../worklet.js.
// The flash the page keeps (as ../worklet.js: storage.c's objects, only the sectors that hold something)
const STORE_AREAS = [[0x9F000, 0xA0000], [0xDC000, 0xE0000], [0xE5000, 0xE7000], [0xFC000, 0xFF000]];
function storeSectors(nor) {
  const out = {};
  for (const [a, b] of STORE_AREAS)
    for (let off = a; off < b; off += 4096) {
      const s = nor.subarray(off, off + 4096);
      if (s.some((v) => v !== 0xFF)) out[off] = s.slice();
    }
  return out;
}
function restoreSectors(nor, sectors) {
  for (const [k, v] of Object.entries(sectors || {})) {
    const off = +k, b = v instanceof Uint8Array ? v : new Uint8Array(v);
    if (STORE_AREAS.some(([a, e]) => off >= a && off + 4096 <= e && off % 4096 === 0) && b.length === 4096)
      nor.set(b, off);
  }
}
const HOLD_MS = 24;

const clock = globalThis.performance ? () => globalThis.performance.now() : () => Date.now();
const nowSec = () => (typeof currentTime === "number" ? currentTime : clock() / 1000);
const Base = typeof AudioWorkletProcessor === "function" ? AudioWorkletProcessor : class { constructor() { this.port = { postMessage() {}, onmessage: null }; } };

export class SMKCore extends Base {
  constructor() {
    super();
    this.ex = null;
    this.sentDraws = -1;
    this.sentWrites = -1;
    this.sentLeds = "";
    this.lastFrame = 0;
    this.period = 1 / 30;
    this.busy = 0;
    this.frames = 0;
    this.queue = [];                                  // keys, pads and buttons, each held >= HOLD_MS of device time
    this.nextOk = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  async onMessage(m) {
    if (m.type === "load") {
      try {
        const { instance } = await WebAssembly.instantiate(m.wasm, {});
        const ex = instance.exports;
        ex._initialize();
        ex.web_nor_erase();
        restoreSectors(new Uint8Array(ex.memory.buffer, ex.web_nor(), ex.web_nor_size()), m.sectors);
        ex.web_boot();
        this.ex = ex;
        this.sentWrites = ex.web_flash_writes_count();
        this.port.postMessage({ type: "ready", rate: ex.web_sample_rate() });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String(err && err.message || err) });
      }
      return;
    }
    const ex = this.ex;
    if (!ex) return;
    if (m.type === "buttons" || m.type === "key" || m.type === "pad") this.queue.push(m);
    else if (m.type === "enc") ex.web_smk_enc(m.e >>> 0, m.n | 0);
    else if (m.type === "fader") ex.web_smk_fader(m.i >>> 0, m.v >>> 0);
    else if (m.type === "wheel") ex.web_smk_wheel(m.i >>> 0, m.raw >>> 0);
    else if (m.type === "midi") ex.web_midi(m.data[0] | 0, m.data[1] | 0, m.data[2] | 0);
    else if (m.type === "visible") this.period = m.on ? 1 / 30 : 1 / 4;
  }

  publish() {
    const ex = this.ex, mem = ex.memory.buffer;
    const msg = { type: "frame" };
    const leds = ex.web_smk_leds();
    const pads = [];
    for (let p = 0; p < 16; p++) pads.push(ex.web_smk_pad_rgb(p));
    const key = leds + ":" + pads.join(",");
    if (key !== this.sentLeds) {
      this.sentLeds = key;
      msg.leds = leds;
      msg.pads = pads;
    }
    if (this.frames >= 44100) {                       // the share of real time spent running the device
      msg.load = this.busy / (this.frames / 44.1);
      this.busy = this.frames = 0;
    }
    const transfer = [];
    const draws = ex.web_screen_draws();
    if (draws !== this.sentDraws) {
      this.sentDraws = draws;
      msg.fb = new Uint16Array(mem, ex.web_screen(), 240 * 240).slice();
      transfer.push(msg.fb.buffer);
    }
    const writes = ex.web_flash_writes_count();
    if (writes !== this.sentWrites) {                 // a save is done within one main-loop pass: whole here
      this.sentWrites = writes;
      msg.sectors = storeSectors(new Uint8Array(mem, ex.web_nor(), ex.web_nor_size()));
    }
    if (msg.leds !== undefined || msg.fb || msg.sectors || msg.load !== undefined) this.port.postMessage(msg, transfer);
  }

  // A press and its release can arrive within one render quantum, but the SMK's keys and pads close for longer than their
  // scan: each event stays HOLD_MS of the device's time before the next goes in (as on the FM-1).
  input() {
    const ex = this.ex, now = ex.web_now_ms();
    while (this.queue.length && now - this.nextOk >= 0) {
      const m = this.queue.shift();
      if (m.type === "buttons") ex.web_smk_buttons(m.mask >>> 0);
      else if (m.type === "key") ex.web_smk_key(m.k >>> 0, m.vel >>> 0);
      else ex.web_smk_pad(m.p >>> 0, m.vel >>> 0);
      this.nextOk = now + HOLD_MS;
    }
  }

  process(inputs, outputs) {
    const ex = this.ex;
    if (!ex) return true;
    if (this.queue.length) this.input();
    const out = outputs[0], n = out[0].length;
    const t0 = clock();
    ex.web_render(n);
    this.busy += clock() - t0;
    this.frames += n;
    out[0].set(new Float32Array(ex.memory.buffer, ex.web_out_l(), n));
    if (out[1]) out[1].set(new Float32Array(ex.memory.buffer, ex.web_out_r(), n));
    if (nowSec() - this.lastFrame >= this.period) {
      this.lastFrame = nowSec();
      this.publish();
    }
    return true;
  }
}
if (typeof registerProcessor === "function") registerProcessor("felucca-smk", SMKCore);
