// SPDX-License-Identifier: GPL-3.0-only
// Evander's Music Box: the emulator page. Draws the FM-1's panel, sends every gesture to the core in the
// AudioWorklet (worklet.js) and shows what it sends back: the screen and the LEDs.

import { SongIO, songHead, download, fileName, WORKBENCH } from "../embmidi.js";

const BTN = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY<br>STOP", "REC", "OCT-", "OCT+"];
const ENC = ["SELECT", "PRESETS", "ALGORITHM", "KNOB1", "KNOB2", "KNOB3", "KNOB4"];
const BLACK_LABEL = ["OP1", "OP2", "OP3", "OP4", "OP5", "OP6", "PIT", "GLO", "MONO", "POLY", ""];
const KEY_OF_WHITE = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const KEY_OF_BLACK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
const BLACK_AFTER_WHITE = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];   // the white key each black key follows
const PC_WHITE = "zxcvbnm,./qwerty", PC_BLACK = "sdfhjl;2346";
const SONG_SLOTS = 17;                                             // 16 scenes + the workbench (sysex slot 16)

const $ = (id) => document.getElementById(id);
let node = null, ctx = null;
const ledEls = new Array(41);
const sysexWait = new Map();
let sysexId = 0;

// ---------------------------------------------------------------- the panel
function el(tag, cls, html) { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; }
function send(m) { if (node) node.port.postMessage(m); }

function encoder(parent, id, label, isPot) {
  const box = el("div");
  const k = el("div", isPot ? "pot" : "enc dark");
  k.tabIndex = 0;
  k.setAttribute("role", "slider");
  k.setAttribute("aria-label", label);
  let rot = isPot ? 90 : 0, dragY = null, acc = 0;
  const turn = (steps) => {
    if (!steps) return;
    if (isPot) {
      rot = Math.max(-135, Math.min(135, rot + steps * 6));
      send({ type: "master", q12: Math.round(((rot + 135) / 270) * 4095) });
    } else {
      rot = (rot + steps * 18) % 360;
      send({ type: "enc", id, steps });
    }
    k.style.setProperty("--rot", rot + "deg");
  };
  k.style.setProperty("--rot", rot + "deg");
  k.addEventListener("wheel", (e) => { e.preventDefault(); turn(e.deltaY < 0 ? 1 : -1); }, { passive: false });
  k.addEventListener("pointerdown", (e) => { dragY = e.clientY; acc = 0; k.setPointerCapture(e.pointerId); });
  k.addEventListener("pointermove", (e) => {
    if (dragY == null) return;
    acc += dragY - e.clientY; dragY = e.clientY;
    while (Math.abs(acc) >= 12) { turn(acc > 0 ? 1 : -1); acc -= Math.sign(acc) * 12; }
  });
  k.addEventListener("pointerup", () => { dragY = null; });
  k.addEventListener("keydown", (e) => {
    if (e.key === "ArrowUp" || e.key === "ArrowRight") { turn(1); e.preventDefault(); }
    if (e.key === "ArrowDown" || e.key === "ArrowLeft") { turn(-1); e.preventDefault(); }
  });
  box.append(k, el("div", "lab", label));
  parent.append(box);
  return turn;
}

function pressable(e, onDown, onUp) {
  const down = (ev) => {
    ev.preventDefault();
    if (ev.ctrlKey || ev.metaKey) {
      if (e.classList.contains("latched")) {
        e.classList.remove("latched", "down");
        onUp();
      } else {
        e.classList.add("latched", "down");
        onDown();
      }
      return;
    }
    if (e.classList.contains("latched")) {
      e.classList.remove("latched", "down");
      onUp();
      return;
    }
    e.setPointerCapture?.(ev.pointerId);
    if (!e.classList.contains("down")) {
      e.classList.add("down");
      onDown();
    }
  };
  const up = () => {
    if (e.classList.contains("latched")) return;
    if (e.classList.contains("down")) {
      e.classList.remove("down");
      onUp();
    }
  };
  const cancel = () => {
    if (e.classList.contains("latched")) return;
    if (e.classList.contains("down")) {
      e.classList.remove("down");
      onUp();
    }
  };
  e.addEventListener("pointerdown", down);
  e.addEventListener("pointerup", up);
  e.addEventListener("pointercancel", cancel);
  e.addEventListener("keydown", (ev) => {
    if ((ev.key === " " || ev.key === "Enter") && !ev.repeat) {
      ev.preventDefault();
      ev.stopPropagation();
      if (!e.classList.contains("down")) {
        e.classList.add("down");
        onDown();
      }
    }
  });
  e.addEventListener("keyup", (ev) => {
    if (ev.key === " " || ev.key === "Enter") {
      ev.stopPropagation();
      if (!e.classList.contains("latched") && e.classList.contains("down")) {
        e.classList.remove("down");
        onUp();
      }
    }
  });
}

function buildPanel() {
  const left = $("left");
  const turners = {};
  turners.master = encoder(left, -1, "MASTER", true);
  turners[0] = encoder(left, 0, "SELECT");
  turners[1] = encoder(left, 1, "PRESETS");
  turners[2] = encoder(left, 2, "ALGORITHM");
  const octs = el("div", "octs");
  for (const id of [12, 13]) {
    const b = el("button", "btn-fm small", BTN[id]);
    b.setAttribute("aria-label", BTN[id]);
    pressable(b, () => send({ type: "button", id, down: 1 }), () => send({ type: "button", id, down: 0 }));
    ledEls[id] = b;
    octs.append(b);
  }
  left.append(octs);
  for (let i = 0; i < 4; i++) {
    encoder($("knobs"), 3 + i, ENC[3 + i]);
  }
  for (let id = 0; id < 12; id++) {
    const b = el("button", "btn-fm", BTN[id]);
    b.setAttribute("aria-label", BTN[id].replace("<br>", "/"));
    pressable(b, () => send({ type: "button", id, down: 1 }), () => send({ type: "button", id, down: 0 }));
    ledEls[id] = b;
    $("btns").append(b);
  }
  for (let w = 0; w < 16; w++) {
    const k = el("div", w % 4 === 0 ? "key group" : "key");   // 1, 5, 9, 13: Encounter's scene groups
    k.tabIndex = 0;
    k.setAttribute("aria-label", `white key ${w + 1}`);
    k.append(el("span", "", ""));
    k.append(el("div", "wnum", String(w + 1)));
    const id = KEY_OF_WHITE[w];
    pressable(k, () => send({ type: "key", id, down: 1, vel: 100 }), () => send({ type: "key", id, down: 0 }));
    ledEls[14 + id] = k;
    $("whites").append(k);
  }
  for (let b = 0; b < 11; b++) {
    const k = el("div", "key");
    k.tabIndex = 0;
    k.setAttribute("aria-label", `black key ${BLACK_LABEL[b] || "top"}`);
    k.style.left = `${((BLACK_AFTER_WHITE[b] + 1) / 16) * 100}%`;   /* between its two white keys */
    k.append(el("span", "", BLACK_LABEL[b]));
    const id = KEY_OF_BLACK[b];
    pressable(k, () => send({ type: "key", id, down: 1, vel: 100 }), () => send({ type: "key", id, down: 0 }));
    ledEls[14 + id] = k;
    $("blacks").append(k);
  }
  return turners;
}

// ---------------------------------------------------------------- the computer keyboard
function bindKeyboard(turners) {
  const held = new Set();
  const fkeys = ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"];
  const action = (e) => {
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    let w = PC_WHITE.indexOf(k), b = PC_BLACK.indexOf(k);
    if (e.key.length === 1 && w >= 0) return { type: "key", id: KEY_OF_WHITE[w] };
    if (e.key.length === 1 && b >= 0) return { type: "key", id: KEY_OF_BLACK[b] };
    const f = fkeys.indexOf(e.key);
    if (f >= 0) return { type: "button", id: f };
    if (e.key === "Escape") return { type: "button", id: 6 };
    if (e.key === " ") return { type: "button", id: 10 };
    if (e.key === "-") return { type: "button", id: 12 };
    if (e.key === "=") return { type: "button", id: 13 };
    return null;
  };
  window.addEventListener("keydown", (e) => {
    if (e.target.closest?.("input, select, .enc, .pot")) return;
    if (e.key === "ArrowLeft" || e.key === "ArrowRight") { turners[0](e.key === "ArrowRight" ? 1 : -1); e.preventDefault(); return; }
    if (e.key === "ArrowUp" || e.key === "ArrowDown") { turners[1](e.key === "ArrowUp" ? 1 : -1); e.preventDefault(); return; }
    const a = action(e);
    if (!a) return;
    e.preventDefault();
    if (e.repeat || held.has(e.code)) return;
    held.add(e.code);
    send({ ...a, down: 1, vel: 100 });
    const tgt = a.type === "button" ? ledEls[a.id] : ledEls[14 + a.id];
    tgt?.classList.add("down");
  });
  window.addEventListener("keyup", (e) => {
    const a = action(e);
    if (!a || !held.has(e.code)) return;
    held.delete(e.code);
    send({ ...a, down: 0 });
    const tgt = a.type === "button" ? ledEls[a.id] : ledEls[14 + a.id];
    tgt?.classList.remove("down");
  });
}

// ---------------------------------------------------------------- storage: the box's flash, in localStorage
const KEY = (slot) => `emb.slot.${slot}`;
function loadStored() {
  const out = {};
  for (let s = 0; s < 2 + 16; s++) {
    const b64 = localStorage.getItem(KEY(s));
    if (b64) try { out[s] = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0)); } catch (_) { /* a broken entry: ignore */ }
  }
  return out;
}
function store(slot, bytes) {
  try { localStorage.setItem(KEY(slot), btoa(String.fromCharCode(...bytes))); } catch (_) { /* private mode: nothing kept */ }
}

// ---------------------------------------------------------------- the screen and the LEDs
const lcd = $("lcd").getContext("2d");
const img = lcd.createImageData(240, 240);
function showFrame(fb) {
  const d = img.data;
  for (let i = 0, j = 0; i < fb.length; i++, j += 4) {
    const c = fb[i];
    d[j] = (c >> 8) & 0xF8; d[j + 1] = (c >> 3) & 0xFC; d[j + 2] = (c << 3) & 0xF8; d[j + 3] = 255;
  }
  lcd.putImageData(img, 0, 0);
}
function showLeds(leds) {
  for (let i = 0; i < 41; i++) {
    const e = ledEls[i];
    if (!e) continue;
    const want = "l" + leds[i];
    if (e.dataset.l !== want) {
      if (e.dataset.l) e.classList.remove(e.dataset.l);
      if (leds[i]) e.classList.add(want);
      e.dataset.l = leds[i] ? want : "";
    }
  }
}

// ---------------------------------------------------------------- SysEx to the emulated box (the console's protocol)
function sysex(body) {
  return new Promise((resolve) => {
    const id = ++sysexId;
    sysexWait.set(id, resolve);
    send({ type: "sysex", id, bytes: Uint8Array.from(body) });
  });
}
const io = new SongIO(sysex);

async function fillSlots() {
  const sel = $("slotSel");
  sel.innerHTML = "";
  for (let s = 0; s < SONG_SLOTS; s++) {
    const h = songHead(await io.read(s, true));
    const o = el("option", "", s === WORKBENCH ? `Workbench: ${h.name}` : `${["REST", "EXPLORE", "COMBAT", "CLIMAX"][s >> 2]} ${["I", "II", "III", "IV"][s & 3]}: ${h.name}`);
    o.value = s;
    sel.append(o);
  }
  sel.value = WORKBENCH;
}

// ---------------------------------------------------------------- start
async function wake() {
  $("wakeBtn").disabled = true;
  try {
    ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
    const bust = "v=1.4.1&t=" + Date.now();
    const [wasm] = await Promise.all([
      fetch("emb.wasm?" + bust, { cache: "no-store" }).then((r) => {
        if (!r.ok) throw new Error("emb.wasm: HTTP " + r.status);
        return r.arrayBuffer();
      }),
      ctx.audioWorklet.addModule("worklet.js?" + bust)
    ]);
    node = new AudioWorkletNode(ctx, "music-box", { numberOfInputs: 0, outputChannelCount: [2], processorOptions: { wasm, stored: loadStored() } });
    node.connect(ctx.destination);
    node.port.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === "frame") {
        if (m.fb) showFrame(m.fb);
        showLeds(m.leds);
        $("meters").textContent = `v1.4.1   voices ${m.voices}   peak ${(20 * Math.log10(Math.max(1, m.peak) / 32768)).toFixed(1)} dBFS   ${ctx.sampleRate} Hz`;
      } else if (m.type === "store") {
        store(m.slot, m.bytes);
      } else if (m.type === "sysex") {
        sysexWait.get(m.id)?.(Array.from(m.bytes));
        sysexWait.delete(m.id);
      }
    };
    await ctx.resume();
    send({ type: "master", q12: Math.round((225 / 270) * 4095) });
    $("wake").remove();
    setTimeout(fillSlots, 4000);                    // after the boot animation
  } catch (e) {
    $("wake").querySelector("p").textContent = "The box would not wake: " + e.message;
    $("wakeBtn").disabled = false;
  }
}

const turners = buildPanel();
bindKeyboard(turners);
$("wakeBtn").addEventListener("click", wake);
$("exportBtn").addEventListener("click", async () => {
  if (!node) return;
  const bytes = await io.read(+$("slotSel").value);
  download(bytes, fileName(songHead(bytes).name, "emb"));
});
$("importBtn").addEventListener("click", () => node && $("importFile").click());
$("importFile").addEventListener("change", async () => {
  const f = $("importFile").files[0];
  if (!f) return;
  try {
    await io.write(+$("slotSel").value, new Uint8Array(await f.arrayBuffer()));
    $("meters").textContent = `imported ${f.name}`;
  } catch (e) { $("meters").textContent = e.message; }
  $("importFile").value = "";
  fillSlots();
});
$("midiBtn").addEventListener("click", async () => {
  try {
    const midi = await navigator.requestMIDIAccess();
    const hook = () => {
      let n = 0;
      for (const i of midi.inputs.values()) { i.onmidimessage = (e) => { const [st, d1 = 0, d2 = 0] = e.data; if (st < 0xF0 || st === 0xFA || st === 0xFC) send({ type: "midi", st, d1, d2 }); }; n++; }
      $("midiState").textContent = n ? `listening to ${n} MIDI input${n > 1 ? "s" : ""}` : "no MIDI inputs found";
    };
    midi.onstatechange = hook;
    hook();
  } catch (e) { $("midiState").textContent = "MIDI unavailable: " + e.message; }
});
