// SPDX-License-Identifier: GPL-3.0-only
// The SMK-37 browser build (docs/smk/emu/felucca.wasm, SMK=1 web/emu/build_wasi.sh), run in node: it boots, draws the screen,
// plays a key and checks the sound, the button LEDs and the pads' colours the page shows.
//   node web/emu/smk_smoke.mjs docs/smk/emu/felucca.wasm
import { readFileSync } from "node:fs";

const file = process.argv[2] || "docs/smk/emu/felucca.wasm";
const { instance } = await WebAssembly.instantiate(readFileSync(file), {});
const ex = instance.exports;
let fails = 0;
const ok = (cond, msg) => { console.log(`smk: ${msg}\t${cond ? "ok" : "FAIL"}`); if (!cond) fails++; };

ex._initialize();
ex.web_nor_erase();
ex.web_boot();
const CH = 128;
const render = (ms) => {                              // as the worklet: 128-frame calls, the device's clock forward
  const frames = Math.round(ms * ex.web_sample_rate() / 1000);
  let peak = 0;
  for (let done = 0; done < frames; done += CH) {
    const n = Math.min(CH, frames - done);
    ex.web_render(n);
    const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), n);
    for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(l[i]));
  }
  return peak;
};
const lit = () => {
  const px = new Uint16Array(ex.memory.buffer, ex.web_screen(), 240 * 240);
  let n = 0;
  for (let i = 0; i < px.length; i++) if (px[i]) n++;
  return n;
};

render(4000);                              // the power-on splash (oa_splash) first: the UI, LEDs and screen after it
ok(ex.web_screen_draws() > 0 && lit() > 500, `the screen draws after the boot (${lit()} lit pixels)`);
ok(ex.web_sample_rate() === 44100, `sample rate 44100 (${ex.web_sample_rate()})`);

ex.web_smk_buttons(1 << 1);                           // FADER BANK: its LED follows within the UI's frames
let leds = 0;
for (let t = 0; t < 25 && leds === 0; t++) { render(60); leds = ex.web_smk_leds(); }
ok(leds !== 0, `a button LED lights (mask 0x${leds.toString(16)})`);
ex.web_smk_buttons(0);

// the key path: a key press reaches the firmware's held notes (C4 = key 12 of the 37, from C3), and its release clears them
const n0 = ex.web_smk_notes();
ex.web_smk_key(12, 100);
render(40);
const n1 = ex.web_smk_notes();
ex.web_smk_key(12, 0);
render(40);
const n2 = ex.web_smk_notes();
ok(n0 === 0 && n1 !== 0 && n2 === 0, `a key reaches the held notes (0x${n0.toString(16)} -> 0x${n1.toString(16)} -> 0x${n2.toString(16)})`);

// the sound: the organ at rest is silent (no drawbars); a registration (the test's heavy organ) plays
const quiet = render(300);
ex.web_test_heavy();
const held = render(300);
ok(held > 0.01 && held > 10 * quiet, `the organ sounds with drawbars out (peak ${held.toFixed(3)}, at rest ${quiet.toFixed(4)})`);


const p0 = ex.web_smk_notes();                        // a pad is a white key too: its hit reaches the held notes
ex.web_smk_pad(0, 100);
render(40);
const p1 = ex.web_smk_notes();
ex.web_smk_pad(0, 0);
render(40);
ok(p0 === 0 && p1 !== 0 && ex.web_smk_notes() === 0, `a pad hit reaches the held notes (0x${p1.toString(16)})`);

ok(Number.isFinite(ex.web_smk_master()), `MASTER (${ex.web_smk_master()})`);
console.log(fails ? `smk: ${fails} FAILED` : "smk: all ok");
process.exit(fails ? 1 : 0);
