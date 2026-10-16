/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Michail Tsakiroglou <https://miketsak.gr>
 * Evander's Music Box: FM-1 LARP Firmware
 *
 * The portable core. Everything musical lives here and builds unchanged for three platforms:
 *   src/platform/fm1.c   the M-VAVE FM-1 (JieLi AC791N / WL82, pi32v2), on Felucca's HAL
 *   src/platform/wasm.c  the browser emulator (wasm32, run inside an AudioWorklet)
 *   src/platform/host.c  the host self-test and WAV renderer
 *
 * Rules the core keeps so the same source is safe on the device:
 *   - integer DSP only (no FPU on the target); no variable '/' or '%' in the core: emb_udiv()
 *   - no heap: every buffer is static; the big ones are placed by EMB_POOL (the device's .pool RAM)
 *   - single writer per datum: the UI side (main loop) talks to the audio side (ISR) only through
 *     the command ring (emb_cmd); the audio side owns voices, decks, DSP state.
 *
 * Units: audio Q15 (32767 = full scale) in int32 with headroom; gains Q15; pitch in MIDI notes.
 */
#pragma once
#include <stdint.h>

#ifndef EMB_POOL
#define EMB_POOL                     /* fm1.c: __attribute__((section(".pool"))) */
#endif
#ifndef EMB_VERSION
#define EMB_VERSION "DEV"            /* tools/build.py passes the VERSION file */
#endif

#define EMB_FS 44100u
#define EMB_CTL 32u                  /* control-rate block, frames (0.73 ms) */
#define EMB_NTRK 4
#define EMB_NSTEP 64
#define EMB_NNOTE 4                  /* notes per step (chords, drum layers) */
#define EMB_NVOICE 24                /* two decks crossfading share this pool */
#define EMB_NSLOT 16                 /* Encounter scene slots = the 16 white keys */
#define EMB_NKEY 27                  /* F3..G5 */
#define EMB_W 240
#define EMB_H 240
#define EMB_TPS 24                   /* clock ticks per step (microtiming resolution) */

/* ---- physical controls (stock FM-1 panel labels) ---- */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO,          /* top row */
       B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,       /* bottom row */
       B_OCTDN, B_OCTUP, B_COUNT };
enum { E_SELECT, E_PRESETS, E_ALGO, E_K1, E_K2, E_K3, E_K4, E_COUNT };
#define EMB_NLED (B_COUNT + EMB_NKEY)                     /* LED ids: buttons, then keys F3..G5 */
enum { LED_OFF, LED_GLOW, LED_MID, LED_LIT };

/* key index (0 = F3) -> is it a white key, and which white key (0..15) / black key (0..10) */
extern const int8_t emb_white_of[EMB_NKEY];               /* -1 for black keys */
extern const int8_t emb_black_of[EMB_NKEY];               /* -1 for white keys */
extern const int8_t emb_key_of_white[16];
extern const int8_t emb_key_of_black[11];
/* the black keys' printed labels: OP1 OP2 OP3 OP4 OP5 OP6 PIT GLO MONO POLY (unlabelled) */
enum { K_OP1, K_OP2, K_OP3, K_OP4, K_OP5, K_OP6, K_PIT, K_GLO, K_MONO, K_POLY, K_TOP };

/* ---- instruments ---- */
enum {                               /* track 1: drum kits */
    I_TAVERN_KIT, I_WAR_KIT, I_RITUAL_KIT,
    /* strings (physical model) */
    I_HARP, I_LUTE, I_NYLON, I_KOTO, I_PSALTERY,
    /* drones and bowed */
    I_CHOIR, I_GURDY, I_BAGPIPE, I_CELLO,
    /* winds and reeds */
    I_PANFLUTE, I_RECORDER, I_SHAWM, I_BRASS,
    /* organs */
    I_ORGANETTO, I_POSITIVE, I_CATHEDRAL,
    /* MODERN MODE (HOME + FX + GLO): the tonewheel organ and the electronic bank */
    I_TONEWHEEL,
    I_SAW_LEAD, I_SQUARE_LEAD, I_SUPERSAW, I_SYNTH_PAD, I_ACID_BASS,   /* subtractive */
    I_FM_EPIANO, I_FM_BELL, I_FM_BASS,                                  /* two-operator FM */
    I_ELECTRO_KIT,                                                      /* a drum machine */
    I_COUNT
};
#define I_FIRST_MODERN I_TONEWHEEL
int emb_inst_is_drum(uint32_t inst);
enum { DR_BODHRAN, DR_FRAME, DR_TAMB, DR_ANVIL, DR_TIMPANI, DR_WAR, DR_CLACK, DR_GONG, DR_COUNT };
extern const char *const emb_inst_name[I_COUNT];
extern const char *const emb_drum_name[DR_COUNT];
extern const char *const emb_drum_name_e[DR_COUNT];   /* the ELECTRO KIT's pieces */

/* ---- scales and temperaments ---- */
enum { M_IONIAN, M_DORIAN, M_PHRYGIAN, M_LYDIAN, M_MIXOLYDIAN, M_AEOLIAN, M_HARMONIC, M_PHRYG_DOM,
       M_DIMINISHED, M_PENTA_MAJ, M_PENTA_MIN, M_COUNT };
enum { T_EQUAL, T_PYTH, T_MEAN, T_JUST, T_COUNT };
extern const char *const emb_mode_name[M_COUNT];
extern const char *const emb_temper_name[T_COUNT];
extern const uint8_t emb_scale[M_COUNT][8];       /* the scale's semitones above the root, emb_scale_n of them */
extern const uint8_t emb_scale_n[M_COUNT];
extern const char *const emb_scale_name[M_COUNT]; /* the friendly names: MAJOR, MINOR, DIMINISHED .. */

/* ---- the song: what a deck plays. Plain bytes: stored to flash and sent over SysEx as is ---- */
typedef struct {
    uint8_t note[EMB_NNOTE];         /* 0 = empty; melodic: MIDI note; drums: piece + 1 */
    uint8_t vel;                     /* 1..127 */
    int8_t micro;                    /* -12..+11 ticks (a step is 24) */
    uint8_t gate;                    /* quarter steps, 1..255 (255: until the track's next note) */
    uint8_t lock;                    /* parameter lock: TONE for this step, 255 = none */
} emb_step_t;

typedef struct {
    emb_step_t s[EMB_NSTEP];
    uint8_t inst, len, level, oct;   /* instrument, length 1..64, level 0..127, (unused: 0) */
    uint8_t tone, chr, rev, pan;     /* TONE, CHARACTER, reverb send 0..127, pan 0..127 */
    uint8_t att, dec, sus, rel;      /* envelope 0..127 */
    uint8_t draw[4];                 /* tonewheel drawbar groups 0..8 (16'+5 1/3', 8'+4', 2 2/3'+2', top) */
} emb_track_t;

#define EMB_SONG_MAGIC 0x424D4545u   /* "EEMB" */
typedef struct {
    uint32_t magic;
    char name[12];
    uint8_t bpm, root, mode, temper; /* bpm 40..240; root 0..11 (0 = C) */
    uint8_t _amb, swing, vib_rate, vib_depth;   /* (_amb: reserved, 0; was the ambience beds) swing 0..12 ticks */
    emb_track_t t[EMB_NTRK];
    uint32_t sum;                    /* emb_sum() of everything before it */
} emb_song_t;

/* ---- settings (stored with the workbench) ---- */
typedef struct {
    uint32_t magic;
    int8_t tune;                     /* A4 calibration, cents -50..+50 (the FM-1's I2S clock runs ~0.7 cent sharp) */
    uint8_t midi_ch, leds, fade_beats;   /* MIDI channel 0..15; LEDs 0 GLOW 1 LIT ONLY 2 STEALTH; crossfade, beats */
    uint8_t quant, rv_size, rv_decay, rv_damp;   /* Encounter quantize 0 beat 1 bar 2 two bars 3 four bars */
    uint8_t rv_mix, snap, out_level, lock_on;   /* out_level: OUTPUT ceiling 0 LOUD (-0.5 dBFS) 1 NORMAL (-3) 2 SAFE (-6) */
    uint8_t lock_root, lock_mode, modern, _r;   /* SCALE LOCK: every note in lock_root lock_mode; MODERN MODE on */
    uint32_t sum;
} emb_settings_t;
#define EMB_SET_MAGIC 0x32455345u    /* "ESE2" */

/* ---- persistence slots (the platform maps them to flash sectors / localStorage keys) ---- */
enum { ST_SETTINGS = 0, ST_WORKBENCH = 1, ST_SCENE0 = 2,      /* scenes: ST_SCENE0 + 0..15 */
       ST_AUTO0 = 18, ST_AUTO_N = 4 };                        /* the autosave ring: ST_AUTO0 + 0..3 */

/* ---- the platform supplies these (main loop only). A slot holds one record of up to 4 KiB ---- */
int emb_hal_erase(uint32_t slot);                              /* 0 = ok; the slot reads as blank after */
int emb_hal_write(uint32_t slot, const void *p, uint32_t n);   /* 0 = ok; into an erased slot */
int emb_hal_load(uint32_t slot, void *p, uint32_t n);          /* 0 = ok and n bytes read */

/* ---- core API ----
 * audio side (ISR / worklet):  emb_render
 * UI side (main loop):         everything else                                                 */
void emb_init(uint32_t seed);                     /* loads settings, workbench, scenes via emb_hal_load */
void emb_render(int32_t *lr, uint32_t frames);    /* interleaved L R, Q15 in int32; frames % EMB_CTL == 0 */
void emb_button(uint32_t b, int down);
void emb_key(uint32_t k, int down, uint32_t vel); /* k 0..26 (F3..G5), vel 1..127 */
void emb_encoder(uint32_t e, int32_t steps);      /* + = clockwise */
void emb_master(uint32_t q12);                    /* the MASTER pot, 0..4095 */
void emb_midi(uint8_t st, uint8_t d1, uint8_t d2);
void emb_tick(uint32_t ms);                       /* main loop, every frame: holds, timeouts, saves */
void emb_leds(uint8_t lv[EMB_NLED]);              /* LED levels (LED_*), blinking already applied */
uint32_t emb_draw(uint16_t *fb, uint32_t row_dirty[(EMB_H + 31) / 32]);   /* RGB565 240x240; 1 if any row changed */
uint32_t emb_sysex(const uint8_t *in, uint32_t n, uint8_t *out);   /* 7-bit body (after F0, before F7) -> reply */
int emb_booting(void);                            /* the boot animation is still running */
void emb_power(uint32_t pct, uint32_t ext);       /* battery 0..100 % (255: unknown), ext: running on USB power */
void emb_boot_note(const char *s);                /* a line shown under the boot animation (the last reset's cause) */
void emb_redraw(void);                            /* the next emb_draw reports every row (the panel was reset) */
void emb_shed(void);                              /* audio side: a render ran long, drop the oldest voice (fm1.c) */

/* ---- shared helpers (emb_dsp.c) ---- */
uint32_t emb_udiv(uint32_t a, uint32_t b);        /* a / b without a divide instruction (control rate only) */
uint32_t emb_sum(const void *p, uint32_t n);
extern int16_t emb_sine[1024];                    /* Q15, filled by emb_init */
void emb_song_clear(emb_song_t *s, const char *name);
void emb_scene_factory(uint32_t slot, emb_song_t *s);   /* emb_scenes.c */
extern const char *const emb_scene_name[EMB_NSLOT];
extern const char *const emb_mod_name[12];

/* ---- core internals shared between the core's files (not for platforms) ---- */
typedef struct {                     /* one command: UI side -> audio side */
    uint8_t op, a, b, c;
    int32_t v;
} emb_cmd_t;
enum { C_NOTE_ON, C_NOTE_OFF, C_PLAY, C_STOP, C_QUEUE, C_PURGE, C_MOD, C_PANIC, C_HOLD, C_LESLIE,
       C_TEMPO, C_FX, C_INTENS, C_DUCK, C_STINGER, C_SEL_TRACK, C_RESYNC, C_CHIME, C_FREESTYLE };
#define TRK_FREE 4u                  /* C_NOTE_ON / C_NOTE_OFF on this "track": Encounter's freestyle voice */
void emb_post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t v);

typedef struct {                     /* what the UI reads back from the audio side (one writer: audio) */
    volatile uint32_t step[2][EMB_NTRK];   /* current step of each deck's tracks */
    volatile uint8_t playing, active, queued, fading;   /* queued: slot+1 (0 none, 255 workbench) */
    volatile uint8_t beat, bar_left, intensity, duck;   /* beat 0..3 within the bar; steps until the switch */
    volatile uint8_t mod_mode, key_shift, rate_t1, hold;   /* hold: bit per track (drone latch) */
    volatile uint8_t leslie, swell, voices, cpu;
    volatile uint8_t deck_slot[2];   /* what each deck plays: slot, 255 workbench, 254 none */
    volatile int8_t play_root;       /* the key the front deck sounds in now (modifiers and lock applied) */
    volatile uint8_t play_mode, freestyle;
    volatile uint32_t beats;         /* beats since power-on (the LED pulse) */
    volatile int16_t peak;           /* output meter */
} emb_status_t;
extern emb_status_t emb_st;
extern emb_song_t emb_bench;         /* the Bardic workbench song */
extern emb_song_t emb_slot[EMB_NSLOT];
extern emb_settings_t emb_set;
extern uint8_t emb_master_vol;       /* 0..255 */

/* sequencer / audio side, used by the UI for previews */
int emb_snap(int note, uint32_t root, uint32_t mode);   /* nearest scale note at or below */
int emb_in_scale(int note, uint32_t root, uint32_t mode);
void emb_audio_init(void);
void emb_track_defaults(emb_track_t *t, uint32_t inst);
