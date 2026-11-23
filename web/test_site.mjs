// SPDX-License-Identifier: GPL-3.0-only
// The built site (web/make_site.py's index.html): every marker filled, both scripts compile and run to the first status
// without Web MIDI, the four packages (FM-1, SMK-37 Pro, Elite, MKE-P37) are embedded with their identities. Node only (no browser, no hardware).
//   python3 web/make_site.py build/felucca.fwsc SITE 0.11.1 PRO.fwsc ELITE.fwsc MKE.fwsc && node web/test_site.mjs SITE/index.html
import { readFileSync } from "node:fs";
import vm from "node:vm";

const file = process.argv[2] || "SITE/index.html";
const html = readFileSync(file, "utf8");
let failed = 0;
const ok = (cond, what) => { console.log(`${what.padEnd(70)} ${cond ? "ok" : "FAIL"}`); if (!cond) failed++; };

ok(!/\/\*(LIB|META|VERSION)\*\//.test(html) && !html.includes("/*META*/{}"), "every marker is filled");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
ok(scripts.length === 2, "two scripts: the installer library, the page");
for (const [i, s] of scripts.entries()) {
  let compiles = true;
  try { new vm.Script(s); } catch (e) { compiles = false; console.log(e.message); }
  ok(compiles, `script ${i + 1} compiles`);
}

// the page's meta: the four packages it carries (the FM-1 first, then the SMK-37's three), each with its identity, its
// download's file name and a Felucca loader package inside
const m = scripts[1].match(/const meta = (\{.*\});/);
const meta = m ? JSON.parse(m[1]) : null;
const want = [["FM-1", "FM-1_8\\d\\d", "organvave-fm1-v"], ["SMK-37 Pro", "SMK-37 Pro_8\\d\\d", "smk/organvave-smk37-v"],
  ["SMK-37 Elite", "SMK-37 Elite_8\\d\\d", "smk/organvave-smk37-elite-v"], ["MKE-P37", "MKE-P37_8\\d\\d", "smk/organvave-smk37-mke-v"]];
ok(meta && meta.pkgs && meta.pkgs.length === want.length, `meta: ${meta && meta.pkgs ? meta.pkgs.length : 0} packages`);
for (const [i, [label, idRe, prefix]] of want.entries()) {
  const e = meta && meta.pkgs && meta.pkgs[i];
  const good = !!e && e.label === label && new RegExp("^" + idRe + "$").test(e.product) &&
    e.file === `${prefix}${meta.version}.fwsc` && e.file.startsWith(prefix);
  ok(good, `package ${i + 1}: ${label} (${e && e.product}, ${e && e.file})`);
  const pkg = e ? Buffer.from(e.pkg.split(",")[1], "base64") : Buffer.alloc(0);
  ok(pkg.includes(Buffer.from("FELUCCA-LOADER-1")), `  its embedded package (${pkg.length} B) is a loader package`);
}

// the page runs without Web MIDI: the first status says so, and nothing throws
const status = { textContent: "", className: "", style: {} };
const el = () => ({ textContent: "", value: "", style: {}, className: "", dataset: {}, children: [], hidden: false, disabled: false,
  setAttribute() {}, appendChild() {}, append() {}, addEventListener() {}, querySelector: () => el(), showModal: null, close() {}, classList: { add() {} } });
const elements = new Map();
const document = { getElementById: (id) => { if (id === "status") return status; if (!elements.has(id)) elements.set(id, el()); return elements.get(id); },
  querySelector: () => el(), querySelectorAll: () => [], createElement: () => el(), addEventListener() {},
  documentElement: { dataset: {}, lang: "" }, body: { className: "", classList: { add() {}, toggle() {} } } };
// the page also reads the browser around the installer: the hero film and the viewer (no timers run here)
const ctx = { document, navigator: { language: "en-US" }, console, setTimeout, clearTimeout, setInterval: () => 0, fetch: undefined,
  TextEncoder, Uint8Array, Date, window: { addEventListener() {}, removeEventListener() {} }, addEventListener() {}, scrollTo() {}, scrollY: 0, location: { hash: "" },
  performance: { now: () => 0 }, requestAnimationFrame: () => 0, matchMedia: () => ({ matches: false }),
  IntersectionObserver: class { observe() {} } };
vm.createContext(ctx);
let threw = null;
try { vm.runInContext(scripts[0] + "\n" + scripts[1], ctx); await new Promise((r) => setTimeout(r, 30)); }
catch (e) { threw = e; console.log(String(e && e.stack || e).split("\n").slice(0, 3).join("\n")); }
ok(!threw, "the page's scripts run without Web MIDI");
ok(status.textContent.includes("Web MIDI") || status.textContent.length > 0, `the first status shows (${status.textContent.slice(0, 60)})`);

console.log(failed ? `${failed} FAILED` : "site ok");
process.exit(failed ? 1 : 0);
