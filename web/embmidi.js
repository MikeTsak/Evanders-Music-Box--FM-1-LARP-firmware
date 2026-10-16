// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Michail Tsakiroglou <https://miketsak.gr>
// Evander's Music Box: the preset protocol (src/emb_ui.c emb_sysex), spoken to the hardware over WebMIDI or
// to the emulator's core. Body between F0 and F7: 7D 45 4D cmd ...
//   01 identity   02 s c get chunk   03 s c data put chunk   04 s factory   06 s cue a scene
// Slots 0..15 are the Encounter scenes (the white keys), 16 the Bardic workbench.

export const HDR = [0x7D, 0x45, 0x4D];
export const WORKBENCH = 16;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function pack7(d) {
  const out = []; let acc = 0, nb = 0;
  for (const b of d) { acc |= b << nb; nb += 8; while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; } }
  if (nb) out.push(acc & 0x7F);
  return out;
}
export function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) { acc |= (b & 0x7F) << nb; nb += 7; while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; } }
  return out;
}

const MODES = ["Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Harmonic minor", "Phrygian dominant"];
const TEMPERS = ["Equal", "Pythagorean", "Meantone", "Just"];
const NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
// the head of a song (emb.h emb_song_t): magic, name[12], bpm, root, mode, temper
export function songHead(b) {
  return {
    name: String.fromCharCode(...b.slice(4, 16)).replace(/\0.*$/, "").trim() || "SONG",
    bpm: b[16], key: `${NOTES[b[17] % 12]} ${MODES[b[18]] || "?"}`, tuning: TEMPERS[b[19]] || "?",
  };
}

// request(body) -> Promise<reply body>; both transports supply one
export class SongIO {
  constructor(request) { this.request = request; this.info = null; }

  async identity() {
    const r = await this.request([...HDR, 1]);
    if (!r || r[4] !== 0x45 || r[5] !== 0x4D || r[6] !== 0x42) throw new Error("no Evander's Box answered");
    this.info = { version: r[7], chunks: r[8], size: r[9] | (r[10] << 7) };
    return this.info;
  }
  async read(slot, onlyHead = false) {
    const { chunks, size } = this.info || await this.identity();
    const n = onlyHead ? 1 : chunks, out = new Uint8Array(chunks * 256);
    for (let c = 0; c < n; c++) {
      const r = await this.request([...HDR, 2, slot, c]);
      if (!r || r[3] !== 2) throw new Error(`slot ${slot}: no answer`);
      out.set(unpack7(r.slice(6)).slice(0, 256), c * 256);
    }
    return out.slice(0, onlyHead ? 256 : size);
  }
  async write(slot, bytes) {
    const { chunks, size } = this.info || await this.identity();
    if (bytes.length !== size) throw new Error(`an Evander's Box song is ${size} bytes, this file has ${bytes.length}`);
    const buf = new Uint8Array(chunks * 256);
    buf.set(bytes);
    for (let c = 0; c < chunks; c++) {
      const r = await this.request([...HDR, 3, slot, c, ...pack7(buf.subarray(c * 256, c * 256 + 256))]);
      if (!r || r[6] !== 0) throw new Error(r && r[6] === 1 ? "the song failed its checksum: not stored" : `chunk ${c} refused`);
    }
  }
  async factory(slot) { await this.request([...HDR, 4, slot]); }
  async cue(slot) { await this.request([...HDR, 6, slot]); }

  // every song in one file: "EMBK", version, count, size (LE16), then the songs
  async backup(onProgress) {
    const { size } = this.info || await this.identity();
    const out = new Uint8Array(8 + 17 * size);
    out.set([0x45, 0x4D, 0x42, 0x4B, 1, 17, size & 255, size >> 8]);
    for (let s = 0; s < 17; s++) {
      out.set(await this.read(s), 8 + s * size);
      onProgress?.(s + 1, 17);
    }
    return out;
  }
  async restore(file, onProgress) {
    const { size } = this.info || await this.identity();
    if (String.fromCharCode(...file.slice(0, 4)) !== "EMBK" || (file[6] | (file[7] << 8)) !== size) throw new Error("not an Evander's Box backup");
    for (let s = 0; s < file[5]; s++) {
      await this.write(s, file.slice(8 + s * size, 8 + (s + 1) * size));
      onProgress?.(s + 1, file[5]);
    }
  }
}

// WebMIDI: find the in/out pair that answers our identity (other gear is never sent anything but it)
export async function connectMidi(log = () => {}) {
  const access = await navigator.requestMIDIAccess({ sysex: true });
  const outs = [...access.outputs.values()];
  for (const input of access.inputs.values()) {
    const output = outs.find((o) => o.name === input.name);
    if (!output) continue;
    const queue = [];
    let waiter = null;
    input.onmidimessage = (e) => {
      if (e.data[0] !== 0xF0) return;
      const body = Array.from(e.data.slice(1, -1));
      if (body[0] !== HDR[0] || body[1] !== HDR[1] || body[2] !== HDR[2]) return;
      queue.push(body);
      if (waiter) { const w = waiter; waiter = null; w(); }
    };
    const request = async (body, ms = 1500) => {
      queue.length = 0;
      output.send([0xF0, ...body, 0xF7]);
      if (!queue.length) await Promise.race([new Promise((r) => { waiter = r; }), sleep(ms)]);
      waiter = null;
      return queue.shift() || null;
    };
    try {
      const io = new SongIO(request);
      await io.identity();
      log(`found Evander's Box on "${input.name}"`);
      return { io, name: input.name, close: () => { input.onmidimessage = null; } };
    } catch (_) {
      input.onmidimessage = null;
    }
  }
  return null;
}

export function download(bytes, name) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}
export const fileName = (name, ext) => `${name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "song"}.${ext}`;
