/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Michail Tsakiroglou <https://miketsak.gr>
 * Evander's Music Box in the browser: the same core as the FM-1, compiled to wasm32 (freestanding, no libc)
 * and run inside an AudioWorklet (web/emu/worklet.js). The worklet calls these exports; storage goes
 * through two imports the worklet supplies (the page keeps the bytes in localStorage).
 * Build: tools/web_build.py */
#include "../emb.c"

#define EXPORT __attribute__((visibility("default")))
#define IMPORT(name) __attribute__((import_module("env"), import_name(name)))

IMPORT("js_store") int js_store(uint32_t slot, const void *p, uint32_t n);
IMPORT("js_load") int js_load(uint32_t slot, void *p, uint32_t n);

/* freestanding: the compiler may call these for struct copies and zeroing loops */
void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *a = d;
    while (n--)
        *a++ = (uint8_t)c;
    return d;
}

int emb_hal_erase(uint32_t slot) { return js_store(slot, 0, 0); }   /* (an empty entry loads as nothing) */
int emb_hal_write(uint32_t slot, const void *p, uint32_t n) { return js_store(slot, p, n); }
int emb_hal_load(uint32_t slot, void *p, uint32_t n) { return js_load(slot, p, n); }

static int32_t audio[2 * 128];
static float audio_f[2 * 128];
static uint16_t fb[EMB_W * EMB_H];
static uint32_t dirty[(EMB_H + 31) / 32];
static uint8_t leds[EMB_NLED];
static uint8_t sx_in[1024], sx_out[1024];

EXPORT void wasm_init(uint32_t seed) { emb_init(seed); }
EXPORT float *wasm_render(uint32_t frames)          /* frames <= 128: planar float L then R */
{
    uint32_t i;
    if (frames > 128u)
        frames = 128u;
    emb_render(audio, frames);
    for (i = 0; i < frames; i++) {
        audio_f[i] = (float)audio[2u * i] * (1.0f / 32768.0f);
        audio_f[128u + i] = (float)audio[2u * i + 1u] * (1.0f / 32768.0f);
    }
    return audio_f;
}
EXPORT void wasm_button(uint32_t b, int down) { emb_button(b, down); }
EXPORT void wasm_key(uint32_t k, int down, uint32_t vel) { emb_key(k, down, vel); }
EXPORT void wasm_encoder(uint32_t e, int32_t steps) { emb_encoder(e, steps); }
EXPORT void wasm_master(uint32_t q12) { emb_master(q12); }
EXPORT void wasm_midi(uint32_t st, uint32_t d1, uint32_t d2) { emb_midi((uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
EXPORT void wasm_tick(uint32_t ms) { emb_tick(ms); }
EXPORT uint32_t wasm_draw(void) { return emb_draw(fb, dirty); }
EXPORT uint16_t *wasm_fb(void) { return fb; }
EXPORT uint8_t *wasm_leds(void)
{
    emb_leds(leds);
    return leds;
}
EXPORT uint8_t *wasm_sysex_in(void) { return sx_in; }
EXPORT uint8_t *wasm_sysex_out(void) { return sx_out; }
EXPORT uint32_t wasm_sysex(uint32_t n) { return emb_sysex(sx_in, n > sizeof sx_in ? sizeof sx_in : n, sx_out); }
EXPORT uint32_t wasm_status(uint32_t i)             /* a few figures for the page */
{
    switch (i) {
    case 0: return emb_st.voices;
    case 1: return (uint32_t)(uint16_t)emb_st.peak;
    case 2: return emb_st.playing;
    default: return 0;
    }
}
