/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: the two modes and every control. UI side (main loop): reads emb_st, edits the
 * workbench, posts commands. The mapping is documented for the bard in MANUAL.md; the short form:
 *
 *   BOTH MODES      HOME + PLAY            switch Bardic Crafting <-> Encounter (the music never stops)
 *                   HOME + REC  (0.4 s)    Bardic: purge the selected track's loop (HOME + SAVE undoes)
 *                                          Encounter: emergency silence
 *                   OCT- + OCT+            octave / intensity reset
 *   BARDIC          keys play the selected track, snapped to the mode; SEL taps the next track
 *                   SEL + OP1..OP4 select a track, SEL + white 1-4 mute; ARP latches a drone
 *                   FX SANCTUARY  ENV ENVELOPE  LFO MOTION (again: Leslie)  EDIT VOICE  GLO MODES  SEQ STEPS
 *                   EDIT + GLO  the modern organ: tonewheel drawbars and the Leslie rotor
 *                   PRESETS instrument, ALGORITHM tempo, SELECT track / page
 *                   SAVE (hold 1 s) save; SAVE + white key (hold) store the workbench as that scene
 *   ENCOUNTER       white keys 1-16 scenes (crossfade on the next bar), black keys modifiers
 *                   K1 intensity  K2 tempo  K3 Sanctuary  K4 music volume; ARP duck; FX cathedral swell
 */

enum { P_HOME, P_FX, P_ENV, P_MOTION, P_VOICE, P_MODE, P_SEQ };
static const char *const PAGE_NAME[7] = {"HOME", "SANCTUARY", "ENVELOPE", "MOTION", "VOICE", "MODES", "STEPS"};
#define REL_BTNS ((1u << B_HOME) | (1u << B_SEL) | (1u << B_EDIT) | (1u << B_GLO) | (1u << B_SAVE))

const int8_t emb_white_of[EMB_NKEY] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12,
                                       -1, 13, 14, -1, 15};
const int8_t emb_black_of[EMB_NKEY] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1,
                                       9, -1, -1, 10, -1};
const int8_t emb_key_of_white[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
const int8_t emb_key_of_black[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

static struct {
    uint8_t mode, page, sub, trk, seq_page, booting, panic_fired;
    int8_t oct, step_held, save_key;
    uint8_t step_used, organ_prev[EMB_NTRK], music_vol, last_scene;
    uint32_t now, boot_t0, btn, used, keys, beat_t, last_beats;
    uint32_t btn_t0[B_COUNT], panic_t0, save_t0;
    int16_t tempo_pm;
    uint8_t key_note[EMB_NKEY], key_trk[EMB_NKEY];  /* the note each held key sent, and to which track */
    uint8_t last_note[EMB_NTRK];
    char msg[28];
    uint32_t msg_t;
    uint32_t row_hash[EMB_H];
    uint8_t save_due;                               /* bit 0 settings+workbench, 1 slots (mask in save_slots) */
    uint16_t save_slots;
} ui;

static void say(const char *m)
{
    uint32_t i;
    for (i = 0; i < sizeof ui.msg - 1u && m[i]; i++)
        ui.msg[i] = m[i];
    ui.msg[i] = 0;
    ui.msg_t = ui.now + 1400u;
}

static int held(uint32_t b) { return (ui.btn >> b) & 1u; }
static emb_track_t *cur_trk(void) { return &emb_bench.t[ui.trk]; }
static int clamp_add(int v, int d, int lo, int hi) { return clampi(v + d, lo, hi); }

/* ------------------------------------------------------------------ persistence */
static void seal_song(emb_song_t *s) { s->sum = emb_sum(s, (uint32_t)((uint8_t *)&s->sum - (uint8_t *)s)); }
static int song_ok(const emb_song_t *s)
{
    return s->magic == EMB_SONG_MAGIC && s->sum == emb_sum(s, (uint32_t)((const uint8_t *)&s->sum - (const uint8_t *)s));
}
static void settings_defaults(void)
{
    uint32_t i;
    for (i = 0; i < sizeof emb_set; i++)
        ((uint8_t *)&emb_set)[i] = 0;
    emb_set.magic = EMB_SET_MAGIC;
    emb_set.fade_beats = 2;
    emb_set.quant = 1;                              /* the next bar */
    emb_set.rv_size = 100;
    emb_set.rv_decay = 92;                          /* ~5 s: stone */
    emb_set.rv_damp = 60;
    emb_set.rv_mix = 44;
    emb_set.snap = 1;
}
static void save_now(void)
{
    uint32_t k;
    if (ui.save_due & 1u) {
        emb_set.sum = emb_sum(&emb_set, (uint32_t)((uint8_t *)&emb_set.sum - (uint8_t *)&emb_set));
        seal_song(&emb_bench);
        emb_hal_store(ST_SETTINGS, &emb_set, sizeof emb_set);
        emb_hal_store(ST_WORKBENCH, &emb_bench, sizeof emb_bench);
    }
    for (k = 0; k < EMB_NSLOT; k++)
        if ((ui.save_slots >> k) & 1u) {
            seal_song(&emb_slot[k]);
            emb_hal_store(ST_SCENE0 + k, &emb_slot[k], sizeof emb_slot[k]);
        }
    ui.save_due = 0;
    ui.save_slots = 0;
}

void emb_init(uint32_t seed)
{
    uint32_t k;
    sine_init();
    nz_state ^= seed;
    for (k = 0; k < sizeof ui; k++)
        ((uint8_t *)&ui)[k] = 0;
    if (emb_hal_load(ST_SETTINGS, &emb_set, sizeof emb_set) || emb_set.magic != EMB_SET_MAGIC ||
        emb_set.sum != emb_sum(&emb_set, (uint32_t)((uint8_t *)&emb_set.sum - (uint8_t *)&emb_set)))
        settings_defaults();
    if (emb_hal_load(ST_WORKBENCH, &emb_bench, sizeof emb_bench) || !song_ok(&emb_bench))
        emb_song_clear(&emb_bench, "WORKBENCH");
    for (k = 0; k < EMB_NSLOT; k++)
        if (emb_hal_load(ST_SCENE0 + k, &emb_slot[k], sizeof emb_slot[k]) || !song_ok(&emb_slot[k]))
            emb_scene_factory(k, &emb_slot[k]);
    emb_audio_init();
    ui.booting = 1;
    ui.step_held = -1;
    ui.save_key = -1;
    ui.music_vol = 127;
    ui.tempo_pm = 1000;
    for (k = 0; k < EMB_NTRK; k++) {
        ui.organ_prev[k] = I_POSITIVE;
        ui.last_note[k] = (uint8_t)(48 + emb_bench.root);
    }
}

int emb_booting(void) { return ui.booting; }

/* ------------------------------------------------------------------ actions */
static void toggle_mode(void)
{
    ui.mode ^= 1u;
    ui.page = P_HOME;
    ui.step_held = -1;
    say(ui.mode ? "ENCOUNTER: TO ARMS" : "BARDIC CRAFTING");
}

static void set_inst(uint32_t t, int inst)
{
    emb_track_t *tr = &emb_bench.t[t];
    emb_track_t keep = *tr;
    uint32_t k;
    emb_track_defaults(tr, (uint32_t)inst);
    for (k = 0; k < EMB_NSTEP; k++)
        tr->s[k] = keep.s[k];
    tr->len = keep.len;
    tr->level = keep.level;
    tr->pan = keep.pan;
    for (k = 0; k < 4u; k++)
        tr->draw[k] = keep.draw[k];
}

static void organ_toggle(void)                      /* EDIT + GLO: pipes <-> tonewheel */
{
    emb_track_t *t = cur_trk();
    if (ui.trk == 0u) {
        say("DRUMS HAVE NO PIPES");
        return;
    }
    if (t->inst == I_TONEWHEEL) {
        set_inst(ui.trk, ui.organ_prev[ui.trk]);
        say("PIPE ORGAN");
    } else {
        ui.organ_prev[ui.trk] = t->inst >= I_ORGANETTO ? t->inst : I_POSITIVE;
        set_inst(ui.trk, I_TONEWHEEL);
        say("TONEWHEEL + LESLIE");
    }
    ui.page = P_VOICE;
}

static void panic_fire(void)
{
    if (ui.mode) {
        emb_post(C_PANIC, 0, 0, 0, 0);
        say("SILENCE");
    } else {
        emb_post(C_PURGE, ui.trk, 0, 0, 0);
        say("LOOP PURGED  HOME+SAVE: UNDO");
    }
}

static int key_note(uint32_t k)                     /* the note a key plays on the selected track */
{
    int w = emb_white_of[k];
    if (ui.trk == 0u)                               /* drums: white keys the pieces, black a ghost of the left one */
        return 1 + ((w >= 0 ? w : emb_white_of[k - 1u]) & 7);
    {
        int n = 53 + (int)k + 12 * ui.oct;
        return emb_set.snap ? emb_snap(n, emb_bench.root, emb_bench.mode) : n;
    }
}

static void step_toggle(uint32_t idx)
{
    emb_step_t *s = &cur_trk()->s[idx];
    if (s->note[0] || s->note[1] || s->note[2] || s->note[3]) {
        s->note[0] = s->note[1] = s->note[2] = s->note[3] = 0;
        s->lock = 255;
        s->micro = 0;
    } else {
        s->note[0] = ui.last_note[ui.trk] ? ui.last_note[ui.trk] : (uint8_t)(ui.trk ? 50 : 1);
        s->vel = s->vel ? s->vel : 100;
        s->gate = s->gate ? s->gate : 4;
    }
}

/* ------------------------------------------------------------------ input */
void emb_button(uint32_t b, int down)
{
    if (b >= B_COUNT)
        return;
    if (ui.booting) {
        if (down && ui.now - ui.boot_t0 > 600u)
            ui.booting = 0;                         /* any button skips the rest of the animation */
        return;
    }
    if (!down) {
        uint32_t was = held(b), used = (ui.used >> b) & 1u, t = ui.now - ui.btn_t0[b];
        ui.btn &= ~(1u << b);
        ui.used &= ~(1u << b);
        if (!was || used)
            return;
        if (b == B_HOME) {
            ui.page = P_HOME;
            ui.step_held = -1;
        } else if (b == B_SEL && !ui.mode) {
            ui.trk = (uint8_t)((ui.trk + 1u) & 3u);
            emb_post(C_SEL_TRACK, ui.trk, 0, 0, 0);
        } else if (b == B_EDIT && !ui.mode) {
            ui.page = ui.page == P_VOICE ? P_HOME : P_VOICE;
        } else if (b == B_GLO && !ui.mode) {
            if (ui.page == P_MODE)
                ui.sub ^= 1u;
            else {
                ui.page = P_MODE;
                ui.sub = 0;
            }
        } else if (b == B_SAVE && t < 1000u) {
            say("HOLD SAVE TO KEEP THY SONG");
        }
        return;
    }
    ui.btn |= 1u << b;
    ui.btn_t0[b] = ui.now;
    /* chords: the second button of a pair */
    if ((b == B_PLAY && held(B_HOME)) || (b == B_HOME && held(B_PLAY))) {
        ui.used |= (1u << B_HOME) | (1u << B_PLAY);
        toggle_mode();
        return;
    }
    if ((b == B_REC && held(B_HOME)) || (b == B_HOME && held(B_REC))) {
        ui.used |= (1u << B_HOME) | (1u << B_REC);
        ui.panic_t0 = ui.now;
        ui.panic_fired = 0;
        return;
    }
    if ((b == B_SAVE && held(B_HOME)) || (b == B_HOME && held(B_SAVE))) {
        ui.used |= (1u << B_HOME) | (1u << B_SAVE);
        if (!ui.mode) {
            emb_post(C_PURGE, ui.trk, 1, 0, 0);
            say("PURGE UNDONE");
        }
        return;
    }
    if ((b == B_GLO && held(B_EDIT)) || (b == B_EDIT && held(B_GLO))) {
        ui.used |= (1u << B_EDIT) | (1u << B_GLO);
        if (!ui.mode)
            organ_toggle();
        return;
    }
    if ((b == B_OCTUP && held(B_OCTDN)) || (b == B_OCTDN && held(B_OCTUP))) {
        ui.oct = 0;
        if (ui.mode)
            emb_post(C_INTENS, 3, 0, 0, 0);
        say(ui.mode ? "FULL HOST" : "OCTAVE 0");
        return;
    }
    if ((REL_BTNS >> b) & 1u)
        return;                                     /* these act on release (they double as modifiers) */
    if (ui.mode) {                                  /* ---- Encounter ---- */
        switch (b) {
        case B_PLAY:
            if (emb_st.playing)
                emb_post(C_STOP, 1, 0, 0, 0);       /* a bar's fade */
            else
                emb_post(C_PLAY, ui.last_scene, 0, 0, 0);
            break;
        case B_ARP:
            emb_post(C_DUCK, emb_st.duck ? 0u : 1u, 0, 0, (int32_t)ui.music_vol * 258);
            say(emb_st.duck ? "MUSIC RISES" : "DUCKED FOR WORDS");
            break;
        case B_FX:
            emb_post(C_FX, 2, 0, 0, 0);
            say(emb_st.swell ? "SANCTUARY" : "CATHEDRAL SWELL");
            break;
        case B_OCTDN:
        case B_OCTUP: {
            int i = clamp_add(emb_st.intensity, b == B_OCTUP ? 1 : -1, 0, 3);
            static const char *const N[4] = {"DRONE ONLY", "+ DRUMS", "+ STRINGS", "FULL HOST"};
            emb_post(C_INTENS, (uint32_t)i, 0, 0, 0);
            say(N[i]);
            break;
        }
        default:
            say("A BARDIC MATTER");
        }
        return;
    }
    switch (b) {                                    /* ---- Bardic ---- */
    case B_FX:
        ui.page = ui.page == P_FX ? P_HOME : P_FX;
        break;
    case B_ENV:
        ui.page = ui.page == P_ENV ? P_HOME : P_ENV;
        break;
    case B_LFO:
        if (ui.page == P_MOTION) {                  /* again: the Leslie's half-moon switch */
            emb_post(C_LESLIE, 0, emb_st.leslie ? 0u : 1u, 0, 0);
            say(emb_st.leslie ? "LESLIE CHORALE" : "LESLIE TREMOLO");
        } else
            ui.page = P_MOTION;
        break;
    case B_SEQ:
        ui.page = ui.page == P_SEQ ? P_HOME : P_SEQ;
        ui.step_held = -1;
        break;
    case B_ARP:
        emb_post(C_HOLD, ui.trk, 0, 0, 0);
        say((emb_st.hold >> ui.trk) & 1u ? "DRONE RELEASED" : "DRONE HELD");
        break;
    case B_PLAY:
        if (emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u)
            emb_post(C_STOP, 0, 0, 0, 0);
        else
            emb_post(C_PLAY, 255, 0, 0, 0);         /* the workbench (after an encounter scene: on the bar) */
        break;
    case B_REC: {
        int on = !((emb_st.hold >> 7) & 1u);
        emb_post(C_RESYNC, (uint32_t)on, 0, 0, 0);
        if (on && !emb_st.playing)
            emb_post(C_PLAY, 255, 0, 0, 0);
        say(on ? "RECORDING THE LOOP" : "LOOP KEPT");
        break;
    }
    case B_OCTDN:
    case B_OCTUP:
        ui.oct = (int8_t)clamp_add(ui.oct, b == B_OCTUP ? 1 : -1, -3, 3);
        break;
    }
}

void emb_key(uint32_t k, int down, uint32_t vel)
{
    int w, bl;
    if (k >= EMB_NKEY || ui.booting)
        return;
    w = emb_white_of[k];
    bl = emb_black_of[k];
    if (!down) {
        ui.keys &= ~(1u << k);
        if (ui.save_key == w)
            ui.save_key = -1;
        if (ui.mode == 0u && ui.page == P_SEQ && w >= 0 && ui.step_held == ui.seq_page * 16 + w) {
            if (!ui.step_used)
                step_toggle((uint32_t)ui.step_held);
            ui.step_held = -1;
        }
        if (ui.key_note[k]) {
            emb_post(C_NOTE_OFF, ui.key_trk[k], ui.key_note[k], 0, 0);
            ui.key_note[k] = 0;
        }
        return;
    }
    ui.keys |= 1u << k;
    if (held(B_SAVE) && w >= 0) {                   /* SAVE + white key: hold to store / to fetch */
        ui.used |= 1u << B_SAVE;
        ui.save_key = (int8_t)w;
        ui.save_t0 = ui.now;
        return;
    }
    if (ui.mode) {                                  /* ---- Encounter ---- */
        if (w >= 0) {
            ui.last_scene = (uint8_t)w;
            emb_post(C_QUEUE, (uint32_t)w, 0, 0, 0);
        } else {
            emb_post(C_MOD, (uint32_t)bl, 0, 0, 0);
            say(emb_mod_name[bl]);
        }
        return;
    }
    if (held(B_SEL)) {                              /* ---- Bardic: track select and mutes ---- */
        ui.used |= 1u << B_SEL;
        if (bl >= 0 && bl < 4) {
            ui.trk = (uint8_t)bl;
            emb_post(C_SEL_TRACK, ui.trk, 0, 0, 0);
        } else if (w >= 0 && w < 4)
            emb_mutes ^= (uint8_t)(1u << w);
        return;
    }
    if (ui.page == P_SEQ) {
        if (bl >= 0 && bl < 4)
            ui.seq_page = (uint8_t)bl;
        else if (w >= 0) {
            ui.step_held = (int8_t)(ui.seq_page * 16 + w);
            ui.step_used = 0;
        }
        return;
    }
    {
        int n = key_note(k);
        ui.key_note[k] = (uint8_t)n;
        ui.key_trk[k] = ui.trk;
        ui.last_note[ui.trk] = (uint8_t)n;
        emb_post(C_NOTE_ON, ui.trk, (uint32_t)n, bl >= 0 && ui.trk == 0u ? 60u : vel, 0);
    }
}

static void knob_song_byte(uint8_t *p, int d, int lo, int hi) { *p = (uint8_t)clamp_add(*p, d, lo, hi); }

static void knob(uint32_t k, int d)                 /* K1..K4 = 0..3 */
{
    emb_track_t *t = cur_trk();
    if (ui.mode) {
        switch (k) {
        case 0: {
            int i = clamp_add(emb_st.intensity, d > 0 ? 1 : -1, 0, 3);
            emb_post(C_INTENS, (uint32_t)i, 0, 0, 0);
            break;
        }
        case 1:
            ui.tempo_pm = (int16_t)clamp_add(ui.tempo_pm, d * 10, 700, 1400);
            emb_post(C_TEMPO, 0, 0, 0, ui.tempo_pm);
            break;
        case 2:
            knob_song_byte(&emb_set.rv_mix, d * 2, 0, 127);
            emb_post(C_FX, 0, 0, 0, 0);
            break;
        case 3:
            ui.music_vol = (uint8_t)clamp_add(ui.music_vol, d * 3, 0, 127);
            emb_post(C_DUCK, emb_st.duck, 0, 0, (int32_t)ui.music_vol * 258);
            break;
        }
        return;
    }
    if (ui.page == P_SEQ && ui.step_held >= 0) {    /* the held step: note, microtiming, velocity, lock */
        emb_step_t *s = &t->s[ui.step_held];
        ui.step_used = 1;
        if (!s->note[0])
            step_toggle((uint32_t)ui.step_held);
        switch (k) {
        case 0:
            if (ui.trk == 0u)
                s->note[0] = (uint8_t)clamp_add(s->note[0], d, 1, 8);
            else {
                int n = s->note[0];
                do
                    n += d > 0 ? 1 : -1;
                while (emb_set.snap && !emb_in_scale(n, emb_bench.root, emb_bench.mode) && n > 1 && n < 126);
                s->note[0] = (uint8_t)clampi(n, 1, 126);
                ui.last_note[ui.trk] = s->note[0];
            }
            break;
        case 1: s->micro = (int8_t)clamp_add(s->micro, d, -12, 11); break;
        case 2: s->vel = (uint8_t)clamp_add(s->vel, d * 4, 1, 127); break;
        case 3:                                     /* the TONE lock; turned below 0 it is gone */
            if (s->lock == 255u) {
                if (d > 0)
                    s->lock = t->tone;
            } else {
                int v = s->lock + d * 2;
                s->lock = (uint8_t)(v < 0 ? 255 : clampi(v, 0, 127));
            }
            break;
        }
        return;
    }
    switch (ui.page) {
    case P_FX:
        knob_song_byte(k == 0 ? &emb_set.rv_size : k == 1 ? &emb_set.rv_decay : k == 2 ? &emb_set.rv_damp : &emb_set.rv_mix,
                       d * 2, 0, 127);
        emb_post(C_FX, 0, 0, 0, 0);
        break;
    case P_ENV:
        knob_song_byte(k == 0 ? &t->att : k == 1 ? &t->dec : k == 2 ? &t->sus : &t->rel, d * 2, 0, 127);
        break;
    case P_MOTION:
        if (k == 0) knob_song_byte(&emb_bench.vib_rate, d * 2, 0, 127);
        if (k == 1) knob_song_byte(&emb_bench.vib_depth, d, 0, 60);
        if (k == 2) knob_song_byte(&emb_bench.swing, d, 0, 12);
        if (k == 3) knob_song_byte(&t->pan, d * 2, 0, 127);
        break;
    case P_VOICE:
        if (t->inst == I_TONEWHEEL)
            knob_song_byte(&t->draw[k], d, 0, 8);
        else
            knob_song_byte(k == 0 ? &t->tone : k == 1 ? &t->chr : k == 2 ? &t->level : &t->rev, d * 2, 0, 127);
        break;
    case P_MODE:
        if (ui.sub == 0u) {
            if (k == 0) emb_bench.root = (uint8_t)((emb_bench.root + 12u + (uint32_t)(d > 0 ? 1 : 11)) % 12u);
            if (k == 1) emb_bench.mode = (uint8_t)((emb_bench.mode + M_COUNT + (d > 0 ? 1u : M_COUNT - 1u)) % M_COUNT);
            if (k == 2) emb_bench.temper = (uint8_t)((emb_bench.temper + T_COUNT + (d > 0 ? 1u : T_COUNT - 1u)) % T_COUNT);
            if (k == 3) emb_set.snap ^= 1u;
        } else {
            if (k == 0) emb_set.tune = (int8_t)clamp_add(emb_set.tune, d, -50, 50);
            if (k == 1) emb_set.midi_ch = (uint8_t)clamp_add(emb_set.midi_ch, d, 0, 15);
            if (k == 2) emb_set.leds = (uint8_t)clamp_add(emb_set.leds, d, 0, 2);
            if (k == 3) emb_set.fade_beats = (uint8_t)clamp_add(emb_set.fade_beats, d, 0, 8);
        }
        break;
    case P_SEQ:
        if (k == 0) knob_song_byte(&t->len, d, 1, EMB_NSTEP);
        if (k == 1) knob_song_byte(&emb_bench.swing, d, 0, 12);
        if (k == 2) knob_song_byte(&t->level, d * 2, 0, 127);
        if (k == 3) knob_song_byte(&t->rev, d * 2, 0, 127);
        break;
    default:                                        /* home: the four tracks' levels */
        knob_song_byte(&emb_bench.t[k].level, d * 2, 0, 127);
        break;
    }
    ui.save_due |= 1u;
}

void emb_encoder(uint32_t e, int32_t steps)
{
    int d = steps > 0 ? 1 : -1;
    if (!steps || ui.booting)
        return;
    while (steps) {
        if (e >= E_K1)
            knob(e - E_K1, d);
        else if (ui.mode) {
            if (e == E_SELECT) {
                emb_set.fade_beats = (uint8_t)clamp_add(emb_set.fade_beats, d, 0, 8);
                say(emb_set.fade_beats ? "CROSSFADE BEATS" : "HARD CUT");
            } else if (e == E_PRESETS) {
                static const char *const Q[4] = {"ON THE BEAT", "ON THE BAR", "EVERY 2 BARS", "EVERY 4 BARS"};
                emb_set.quant = (uint8_t)clamp_add(emb_set.quant, d, 0, 3);
                say(Q[emb_set.quant]);
            } else {
                ui.tempo_pm = (int16_t)clamp_add(ui.tempo_pm, d * 20, 700, 1400);
                emb_post(C_TEMPO, 0, 0, 0, ui.tempo_pm);
            }
        } else if (e == E_SELECT) {
            if (ui.page == P_SEQ && ui.step_held >= 0) {   /* with a step held: its gate */
                emb_step_t *s = &cur_trk()->s[ui.step_held];
                s->gate = (uint8_t)clamp_add(s->gate, d, 1, 255);
                ui.step_used = 1;
            } else if (ui.page == P_SEQ)
                ui.seq_page = (uint8_t)clamp_add(ui.seq_page, d, 0, 3);
            else if (ui.page == P_MODE)
                ui.sub ^= 1u;
            else {
                ui.trk = (uint8_t)((ui.trk + (uint32_t)(d > 0 ? 1 : 3)) & 3u);
                emb_post(C_SEL_TRACK, ui.trk, 0, 0, 0);
            }
        } else if (e == E_PRESETS) {
            int lo = ui.trk ? I_FIRST_MELODIC : 0, hi = ui.trk ? I_COUNT - 1 : I_RITUAL_KIT;
            int i = cur_trk()->inst + d;
            set_inst(ui.trk, i < lo ? hi : i > hi ? lo : i);
            say(emb_inst_name[cur_trk()->inst]);
            ui.save_due |= 1u;
        } else {
            emb_bench.bpm = (uint8_t)clamp_add(emb_bench.bpm, d, 40, 240);
            ui.save_due |= 1u;
        }
        steps -= d;
    }
}

void emb_master(uint32_t q12) { emb_master_vol = (uint8_t)(q12 >> 4); }

void emb_midi(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t ch = st & 15u, ty = st & 0xF0u;
    if (st == 0xFAu) {
        emb_post(C_PLAY, ui.mode ? ui.last_scene : 255u, 0, 0, 0);
        return;
    }
    if (st == 0xFCu) {
        emb_post(C_STOP, ui.mode, 0, 0, 0);
        return;
    }
    if (ch != emb_set.midi_ch)
        return;
    if (ty == 0xC0u && d1 < EMB_NSLOT) {            /* program change 1..16: cue a scene (a GM's foot switch) */
        ui.last_scene = d1;
        emb_post(C_QUEUE, d1, 0, 0, 0);
    } else if (ty == 0x90u && d2) {
        if (ui.mode && d1 >= 36u && d1 < 52u)
            emb_post(C_QUEUE, d1 - 36u, 0, 0, 0);
        else if (ui.mode && d1 >= 52u && d1 < 63u)
            emb_post(C_MOD, d1 - 52u, 0, 0, 0);
        else if (!ui.mode)
            emb_post(C_NOTE_ON, ui.trk, d1, d2, 0);
    } else if (ty == 0x80u || ty == 0x90u) {
        if (!ui.mode)
            emb_post(C_NOTE_OFF, ui.trk, d1, 0, 0);
    } else if (ty == 0xB0u && d1 == 7u) {
        ui.music_vol = d2;
        emb_post(C_DUCK, emb_st.duck, 0, 0, (int32_t)d2 * 258);
    }
}

void emb_tick(uint32_t ms)
{
    ui.now = ms;
    if (!ui.boot_t0)
        ui.boot_t0 = ms | 1u;
    if (ui.booting && ms - ui.boot_t0 > 3400u)
        ui.booting = 0;
    if (emb_st.beats != ui.last_beats) {
        ui.last_beats = emb_st.beats;
        ui.beat_t = ms;
    }
    /* HOME + REC held: the purge / silence fires after 0.4 s, once */
    if (held(B_HOME) && held(B_REC) && !ui.panic_fired && ms - ui.panic_t0 >= 400u) {
        ui.panic_fired = 1;
        panic_fire();
    }
    /* SAVE held alone a second: keep everything */
    if (held(B_SAVE) && !((ui.used >> B_SAVE) & 1u) && ms - ui.btn_t0[B_SAVE] >= 1000u) {
        ui.used |= 1u << B_SAVE;
        ui.save_due |= 1u;
        save_now();
        say("MOTHER MELODY KEEPS IT");
    }
    /* SAVE + white key held 0.8 s: Bardic stores the workbench as that scene, Encounter fetches the scene */
    if (ui.save_key >= 0 && held(B_SAVE) && ms - ui.save_t0 >= 800u) {
        uint32_t s = (uint32_t)ui.save_key;
        if (ui.mode) {
            emb_bench = emb_slot[s];
            say("SCENE TO WORKBENCH");
            ui.save_due |= 1u;
        } else {
            emb_slot[s] = emb_bench;
            ui.save_slots |= (uint16_t)(1u << s);
            say("SONG BOUND TO KEY");
        }
        save_now();
        ui.save_key = -1;
    }
}

/* ------------------------------------------------------------------ LEDs */
void emb_leds(uint8_t lv[EMB_NLED])
{
    uint32_t i, now = ui.now, fast = (now >> 6) & 1u, slow = (now >> 8) & 1u;
    uint32_t beat = now - ui.beat_t < 110u, style = emb_set.leds;
    uint8_t glow = style ? LED_OFF : LED_GLOW;
    for (i = 0; i < EMB_NLED; i++)
        lv[i] = LED_OFF;
    if (ui.booting)
        return;                                     /* (the platform's own power-on sweep runs) */
    if (held(B_HOME) && held(B_REC)) {              /* arming the purge: both blink fast */
        lv[B_HOME] = lv[B_REC] = fast ? LED_LIT : LED_OFF;
        return;
    }
    lv[B_PLAY] = emb_st.playing ? (beat ? LED_LIT : glow) : LED_OFF;
    if (ui.mode) {                                  /* ---- Encounter ---- */
        uint32_t w, playing_slot = emb_st.deck_slot[emb_st.active];
        lv[B_HOME] = style == 2u ? LED_OFF : LED_MID;   /* the steady half light says "Encounter" */
        lv[B_ARP] = emb_st.duck ? LED_LIT : glow;
        lv[B_FX] = emb_st.swell ? LED_LIT : glow;
        lv[B_OCTDN] = lv[B_OCTUP] = glow;
        for (w = 0; w < 16u; w++) {
            uint8_t l = (w & 3u) == 0u ? LED_MID : glow;   /* the first key of each four: a landmark */
            if (style == 2u)
                l = LED_OFF;
            if (emb_st.playing && playing_slot == w)
                l = LED_LIT;
            if (emb_st.queued == w + 1u)
                l = fast ? LED_LIT : LED_OFF;
            lv[B_COUNT + emb_key_of_white[w]] = l;
        }
        for (w = 0; w < 11u; w++) {
            uint8_t on = 0;
            switch (w) {
            case 0: on = emb_st.mod_mode == M_PHRYGIAN; break;
            case 2: on = emb_st.mod_mode == M_HARMONIC; break;
            case 3: on = emb_st.mod_mode == M_IONIAN; break;
            case 4: on = (int8_t)emb_st.key_shift > 0; break;
            case 5: on = (int8_t)emb_st.key_shift < 0; break;
            case 6: on = emb_st.rate_t1 == 4u; break;
            case 7: on = emb_st.rate_t1 == 1u; break;
            case 8: on = (emb_st.hold >> 6) & 1u; break;
            }
            lv[B_COUNT + emb_key_of_black[w]] = on ? LED_LIT : glow;
        }
        return;
    }
    /* ---- Bardic ---- */
    lv[B_HOME] = ui.page == P_HOME ? LED_LIT : glow;
    lv[B_FX] = ui.page == P_FX ? LED_LIT : glow;
    lv[B_ENV] = ui.page == P_ENV ? LED_LIT : glow;
    lv[B_LFO] = ui.page == P_MOTION ? (emb_st.leslie && slow ? LED_MID : LED_LIT) : glow;
    lv[B_EDIT] = ui.page == P_VOICE ? LED_LIT : glow;
    lv[B_GLO] = ui.page == P_MODE ? LED_LIT : glow;
    lv[B_SEQ] = ui.page == P_SEQ ? LED_LIT : glow;
    lv[B_SEL] = glow;
    lv[B_SAVE] = held(B_SAVE) ? LED_LIT : glow;
    lv[B_ARP] = (emb_st.hold >> ui.trk) & 1u ? LED_LIT : glow;
    lv[B_REC] = (emb_st.hold >> 7) & 1u ? (emb_st.playing ? LED_LIT : (slow ? LED_LIT : LED_OFF)) : glow;
    {
        int o = ui.oct < 0 ? -ui.oct : ui.oct;
        uint8_t l = o == 1 ? LED_MID : o == 2 ? (slow ? LED_LIT : LED_OFF) : o == 3 ? LED_LIT : LED_OFF;
        lv[ui.oct < 0 ? B_OCTDN : B_OCTUP] = l;
    }
    if (held(B_SEL)) {                              /* the track picker */
        for (i = 0; i < 4u; i++) {
            lv[B_COUNT + emb_key_of_black[i]] = i == ui.trk ? LED_LIT : glow;
            lv[B_COUNT + emb_key_of_white[i]] = (emb_mutes >> i) & 1u ? LED_OFF : LED_LIT;
        }
        return;
    }
    if (ui.page == P_SEQ) {
        const emb_track_t *t = cur_trk();
        uint32_t ph = emb_st.step[emb_st.active][ui.trk], live = emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u;
        for (i = 0; i < 16u; i++) {
            uint32_t idx = ui.seq_page * 16u + i;
            const emb_step_t *s = &t->s[idx];
            uint8_t l = idx >= t->len ? LED_OFF :
                        (s->note[0] || s->note[1] || s->note[2] || s->note[3]) ?
                        ((s->lock != 255u || s->micro) ? LED_MID : LED_LIT) : glow;
            if (live && idx == ph)
                l = fast ? (l == LED_LIT ? LED_OFF : LED_LIT) : l;   /* the playhead: inverted, blinking */
            lv[B_COUNT + emb_key_of_white[i]] = l;
        }
        for (i = 0; i < 4u; i++)
            lv[B_COUNT + emb_key_of_black[i]] = i == ui.seq_page ? LED_LIT :
                                               (live && (ph >> 4) == i) ? LED_MID : glow;
        return;
    }
    for (i = 0; i < EMB_NKEY; i++) {                /* playing: the scale glows, the roots half lit */
        int n = 53 + (int)i + 12 * ui.oct;
        uint8_t l;
        if (ui.trk == 0u)
            l = emb_white_of[i] >= 0 && emb_white_of[i] < 8 ? glow : LED_OFF;
        else if (!emb_set.snap || emb_in_scale(n, emb_bench.root, emb_bench.mode))
            l = (uint32_t)(n - (int)emb_bench.root + 120) % 12u == 0u ? LED_MID : glow;
        else
            l = LED_OFF;
        if ((ui.keys >> i) & 1u)
            l = LED_LIT;
        lv[B_COUNT + i] = l;
    }
}

/* ------------------------------------------------------------------ screens */
static void top_bar(const char *left, const char *right, uint16_t accent)
{
    fill(0, 0, EMB_W, 22, COL_NAVY2);
    fill(0, 22, EMB_W, 1, COL_WOOD);
    fill(0, 23, EMB_W, 1, COL_PARCH);
    disc(11, 11, 6, accent);
    disc(11, 11, 3, COL_NAVY2);
    text(22, 7, left, 1, COL_PARCH);
    text(EMB_W - 4 - text_w(right, 1), 7, right, 1, accent);
}
static void bottom_bar(const char *const lab[4])
{
    int k;
    fill(0, EMB_H - 20, EMB_W, 1, COL_WOOD);
    fill(0, EMB_H - 19, EMB_W, 19, COL_NAVY2);
    for (k = 0; k < 4; k++)
        text_c(30 + k * 60, EMB_H - 13, lab[k], 1, COL_CYAN);
}
static void gauge(int x, int y, const char *label, const char *val, int v, int vmax)   /* one knob */
{
    int k, a = (int)emb_udiv((uint32_t)(v * 600), (uint32_t)(vmax ? vmax : 1));   /* 0..600 of 1024: a 210 deg arc */
    for (k = 0; k <= 600; k += 15) {
        int ang = 1024 - 300 + k;
        disc(x + pol_x(ang, 22), y + pol_y(ang, 22), 2, k <= a ? COL_CYAN : COL_CYAN_D);
    }
    disc(x + pol_x(1024 - 300 + a, 14), y + pol_y(1024 - 300 + a, 14), 3, COL_PARCH);
    text_c(x, y - 4, val, 1, COL_WHITE);
    text_c(x, y + 28, label, 1, COL_PARCH);
}

static void draw_message(void)
{
    if (!ui.msg[0] || (int32_t)(ui.now - ui.msg_t) > 0)
        return;
    fill(10, 100, 220, 34, COL_NAVY);
    frame(10, 100, 220, 34, COL_PARCH);
    frame(12, 102, 216, 30, COL_WOOD);
    text_c(120, 113, ui.msg, 1, COL_CYAN);
}

static const char *const FAMILY[4] = {"REST", "EXPLORE", "COMBAT", "CLIMAX"};
static const uint16_t FAMILY_COL[4] = {COL_PARCH, RGB(0x6C, 0xD6, 0x8A), COL_EMBER, RGB(0xE0, 0x40, 0x60)};

static void draw_encounter(void)
{
    char b[32];
    uint32_t playing_slot = emb_st.deck_slot[emb_st.active], w, cur = emb_st.playing && playing_slot < 16u ? playing_slot : 255u;
    static const char *const LAB[4] = {"INTENS", "TEMPO", "HALL", "VOLUME"};
    b[0] = 0;
    num(b, (int32_t)emb_udiv((cur < 16u ? emb_slot[cur].bpm : 96u) * (uint32_t)ui.tempo_pm, 1000u));
    cat(b, " BPM");
    top_bar("ENCOUNTER", b, COL_EMBER);
    if (cur < 16u) {
        text_glow(120, 34, emb_slot[cur].name, 3, COL_WHITE, COL_NAVY2);
        text_c(120, 60, FAMILY[cur >> 2], 1, FAMILY_COL[cur >> 2]);
    } else {
        text_glow(120, 34, emb_st.playing ? "THE BARD PLAYS" : "SILENCE", 2, COL_DIM, COL_NAVY);
    }
    for (w = 0; w < 16u; w++) {                     /* the scene tiles, as the keys lie */
        int x = 6 + (int)(w & 3u) * 58, y = 74 + (int)(w >> 2) * 24;
        uint16_t c = FAMILY_COL[w >> 2], bg = COL_NAVY2;
        char t[8];
        uint32_t i;
        if (w == cur)
            bg = c;
        if (emb_st.queued == w + 1u && ((ui.now >> 7) & 1u))
            bg = COL_CYAN;
        fill(x, y, 54, 20, bg);
        frame(x, y, 54, 20, c);
        for (i = 0; i < 7u && emb_slot[w].name[i]; i++)
            t[i] = emb_slot[w].name[i];
        t[i] = 0;
        text_c(x + 27, y + 7, t, 1, bg == COL_NAVY2 ? c : COL_NAVY);
    }
    if (emb_st.queued) {                            /* the countdown to the switch */
        b[0] = 0;
        cat(b, "ON THE MARK: ");
        {
            char n[6];
            num(n, (int32_t)(emb_st.bar_left ? emb_st.bar_left : 16u));
            cat(b, n);
        }
        text_c(120, 174, b, 1, COL_CYAN);
    } else {
        b[0] = 0;
        if (emb_st.mod_mode != 255u)
            cat(b, emb_mode_name[emb_st.mod_mode]);
        if ((int8_t)emb_st.key_shift) {
            char n[6];
            cat(b, b[0] ? " " : "");
            num(n, (int8_t)emb_st.key_shift);
            cat(b, n[0] == '-' ? "" : "+");
            cat(b, n);
        }
        if (emb_st.rate_t1 != 2u)
            cat(b, emb_st.rate_t1 == 4u ? " DOUBLE" : " HALF");
        if ((emb_st.hold >> 6) & 1u)
            cat(b, " BREAK");
        text_c(120, 174, b[0] ? b : "MOTHER MELODY HEARS", 1, b[0] ? COL_EMBER : COL_DIM);
    }
    for (w = 0; w < 4u; w++)                        /* intensity */
        fill(60 + (int)w * 31, 188, 28, 10, w <= emb_st.intensity ? FAMILY_COL[w] : COL_NAVY2);
    if (emb_st.duck)
        text_c(120, 204, "DUCKED", 1, COL_CYAN);
    bottom_bar(LAB);
}

static void draw_bardic_home(void)
{
    char b[32];
    uint32_t t, i;
    static const char *const LAB[4] = {"LVL 1", "LVL 2", "LVL 3", "LVL 4"};
    b[0] = 0;
    num(b, emb_bench.bpm);
    cat(b, " BPM");
    top_bar("BARDIC CRAFTING", b, COL_CYAN);
    for (t = 0; t < EMB_NTRK; t++) {
        const emb_track_t *tr = &emb_bench.t[t];
        int y = 30 + (int)t * 40;
        uint32_t ph = emb_st.step[emb_st.active][t], live = emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u;
        uint32_t page = live ? ph >> 4 : 0u;
        if (t == ui.trk)
            frame(2, y - 2, 236, 38, COL_PARCH);
        b[0] = (char)('1' + t);
        b[1] = 0;
        text(6, y + 2, b, 2, (emb_mutes >> t) & 1u ? COL_DIM : COL_CYAN);
        text(22, y + 2, emb_inst_name[tr->inst], 1, (emb_mutes >> t) & 1u ? COL_DIM : COL_PARCH);
        if ((emb_st.hold >> t) & 1u)
            text(EMB_W - 40, y + 2, "HOLD", 1, COL_EMBER);
        for (i = 0; i < 16u; i++) {
            uint32_t idx = page * 16u + i;
            const emb_step_t *s = &tr->s[idx];
            int x = 22 + (int)i * 13;
            uint16_t c = idx >= tr->len ? COL_NAVY : (s->note[0] || s->note[1] || s->note[2] || s->note[3]) ? COL_CYAN : COL_NAVY2;
            fill(x, y + 14, 11, 16, c);
            if (live && idx == ph)
                frame(x - 1, y + 13, 13, 18, COL_PARCH);
        }
    }
    b[0] = 0;
    {
        char n[8];
        note_name(n, emb_bench.root);
        n[n[1] == '#' ? 2 : 1] = 0;
        cat(b, n);
    }
    cat(b, " ");
    cat(b, emb_mode_name[emb_bench.mode]);
    cat(b, "  ");
    cat(b, emb_temper_name[emb_bench.temper]);
    text_c(120, 194, b, 1, COL_PARCH);
    if ((emb_st.hold >> 7) & 1u)
        disc(14, 197, 4, (ui.now >> 8) & 1u ? COL_RED : COL_NAVY2);
    if (ui.oct) {
        b[0] = 'O';
        b[1] = 'C';
        b[2] = 'T';
        b[3] = ' ';
        num(b + 4, ui.oct);
        text(EMB_W - 46, 194, b, 1, COL_DIM);
    }
    bottom_bar(LAB);
}

static void draw_seq(void)
{
    const emb_track_t *t = cur_trk();
    char b[32];
    uint32_t i, ph = emb_st.step[emb_st.active][ui.trk], live = emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u;
    static const char *const LAB[4] = {"LENGTH", "SWING", "LEVEL", "REVERB"};
    static const char *const LAB_H[4] = {"NOTE", "TIMING", "VELO", "LOCK"};
    b[0] = 'T';
    b[1] = (char)('1' + ui.trk);
    b[2] = ' ';
    b[3] = 0;
    cat(b, "STEPS ");
    {
        char n[4];
        num(n, ui.seq_page + 1);
        cat(b, n);
    }
    cat(b, "/4");
    top_bar(b, emb_inst_name[t->inst], COL_CYAN);
    for (i = 0; i < 16u; i++) {
        uint32_t idx = ui.seq_page * 16u + i;
        const emb_step_t *s = &t->s[idx];
        int x = 4 + (int)(i & 3u) * 58, y = 30 + (int)(i >> 2) * 36;
        uint16_t bg = idx >= t->len ? COL_NAVY : COL_NAVY2;
        if (s->note[0])
            bg = COL_ROSE_D;
        fill(x, y, 54, 32, bg);
        frame(x, y, 54, 32, (int)idx == ui.step_held ? COL_PARCH : (live && idx == ph) ? COL_CYAN : COL_WOOD);
        if (s->note[0]) {
            char n[8];
            if (ui.trk == 0u) {
                const char *dn = emb_drum_name[(s->note[0] - 1u) & 7u];
                n[0] = dn[0];
                n[1] = dn[1];
                n[2] = dn[2];
                n[3] = 0;
            } else
                note_name(n, s->note[0]);
            text(x + 4, y + 4, n, 1, COL_WHITE);
            if (s->note[1])
                text(x + 34, y + 4, "+", 1, COL_CYAN);
            if (s->micro)
                fill(x + 27 + s->micro, y + 24, 3, 4, COL_EMBER);   /* the nudge */
            if (s->lock != 255u)
                disc(x + 47, y + 25, 3, COL_GOLD);
        }
    }
    if (ui.step_held >= 0) {
        const emb_step_t *s = &t->s[ui.step_held];
        char n[8];
        b[0] = 0;
        cat(b, "VEL ");
        num(n, s->vel);
        cat(b, n);
        cat(b, "  GATE ");
        num(n, s->gate);
        cat(b, n);
        cat(b, "  NUDGE ");
        num(n, s->micro);
        cat(b, n);
        text_c(120, 178, b, 1, COL_PARCH);
    }
    bottom_bar(ui.step_held >= 0 ? LAB_H : LAB);
}

static void draw_page(void)
{
    emb_track_t *t = cur_trk();
    const char *lab[4];
    char val[4][12];
    int v[4], vmax[4], k;
    char title[24];
    title[0] = 'T';
    title[1] = (char)('1' + ui.trk);
    title[2] = ' ';
    title[3] = 0;
    cat(title, PAGE_NAME[ui.page]);
    for (k = 0; k < 4; k++) {
        lab[k] = "";
        v[k] = 0;
        vmax[k] = 127;
    }
    switch (ui.page) {
    case P_FX:
        lab[0] = "SIZE"; lab[1] = "DECAY"; lab[2] = "DAMP"; lab[3] = "MIX";
        v[0] = emb_set.rv_size; v[1] = emb_set.rv_decay; v[2] = emb_set.rv_damp; v[3] = emb_set.rv_mix;
        title[0] = 0;
        cat(title, "SANCTUARY REVERB");
        break;
    case P_ENV:
        lab[0] = "ATTACK"; lab[1] = "DECAY"; lab[2] = "SUSTAIN"; lab[3] = "RELEASE";
        v[0] = t->att; v[1] = t->dec; v[2] = t->sus; v[3] = t->rel;
        break;
    case P_MOTION:
        lab[0] = "VIB RATE"; lab[1] = "VIB DEPTH"; lab[2] = "SWING"; lab[3] = "PAN";
        v[0] = emb_bench.vib_rate; v[1] = emb_bench.vib_depth; vmax[1] = 60; v[2] = emb_bench.swing; vmax[2] = 12;
        v[3] = t->pan;
        break;
    case P_VOICE:
        if (t->inst == I_TONEWHEEL) {
            lab[0] = "16' 5'"; lab[1] = "8' 4'"; lab[2] = "2' MIX"; lab[3] = "TOP";
            for (k = 0; k < 4; k++) {
                v[k] = t->draw[k];
                vmax[k] = 8;
            }
        } else {
            lab[0] = "TONE"; lab[1] = "CHARACTER"; lab[2] = "LEVEL"; lab[3] = "REVERB";
            v[0] = t->tone; v[1] = t->chr; v[2] = t->level; v[3] = t->rev;
        }
        break;
    case P_MODE:
        if (ui.sub == 0u) {
            lab[0] = "ROOT"; lab[1] = "MODE"; lab[2] = "TUNING"; lab[3] = "SNAP";
            v[0] = emb_bench.root; vmax[0] = 11; v[1] = emb_bench.mode; vmax[1] = M_COUNT - 1;
            v[2] = emb_bench.temper; vmax[2] = T_COUNT - 1; v[3] = emb_set.snap; vmax[3] = 1;
        } else {
            lab[0] = "A4 CAL"; lab[1] = "MIDI CH"; lab[2] = "LEDS"; lab[3] = "X-FADE";
            v[0] = emb_set.tune + 50; vmax[0] = 100; v[1] = emb_set.midi_ch; vmax[1] = 15;
            v[2] = emb_set.leds; vmax[2] = 2; v[3] = emb_set.fade_beats; vmax[3] = 8;
        }
        break;
    }
    for (k = 0; k < 4; k++)
        num(val[k], v[k]);
    if (ui.page == P_MODE && ui.sub == 0u) {        /* names instead of numbers */
        note_name(val[0], emb_bench.root);
        val[0][val[0][1] == '#' ? 2 : 1] = 0;
        {
            static const char *const MS[M_COUNT] = {"ION", "DOR", "PHR", "LYD", "MIX", "AEO", "HARM", "PDOM"};
            static const char *const TS[T_COUNT] = {"EQ", "PYTH", "MEAN", "JUST"};
            val[1][0] = val[2][0] = 0;
            cat(val[1], MS[emb_bench.mode]);
            cat(val[2], TS[emb_bench.temper]);
        }
        val[3][0] = emb_set.snap ? 'O' : '-';
        val[3][1] = emb_set.snap ? 'N' : 0;
        val[3][2] = 0;
    }
    if (ui.page == P_MODE && ui.sub == 1u)
        num(val[0], emb_set.tune);
    top_bar(title, emb_inst_name[t->inst], COL_CYAN);
    for (k = 0; k < 4; k++)
        gauge(60 + (k & 1) * 120, 66 + (k >> 1) * 80, lab[k], val[k], v[k], vmax[k]);
    if (ui.page == P_MODE && ui.sub == 0u) {
        char b[40];
        b[0] = 0;
        cat(b, emb_mode_name[emb_bench.mode]);
        cat(b, " - ");
        cat(b, emb_temper_name[emb_bench.temper]);
        text_c(120, 202, b, 1, COL_PARCH);
    }
    if (ui.page == P_MOTION && t->inst == I_TONEWHEEL)
        text_c(120, 202, emb_st.leslie ? "LESLIE: TREMOLO (LFO)" : "LESLIE: CHORALE (LFO)", 1, COL_PARCH);
    bottom_bar(lab);
}

uint32_t emb_draw(uint16_t *fb, uint32_t row_dirty[(EMB_H + 31) / 32])
{
    uint32_t y, any = 0;
    G = fb;
    if (ui.booting) {
        boot_frame(ui.now - ui.boot_t0);
    } else {
        fill(0, 0, EMB_W, EMB_H, COL_NAVY);
        if (ui.mode)
            draw_encounter();
        else if (ui.page == P_HOME)
            draw_bardic_home();
        else if (ui.page == P_SEQ)
            draw_seq();
        else
            draw_page();
        if (held(B_HOME) && held(B_REC) && !ui.panic_fired) {   /* the purge ring fills while held */
            uint32_t p = emb_udiv((ui.now - ui.panic_t0) * 1024u, 400u), a;
            fill(40, 70, 160, 100, COL_NAVY);
            frame(40, 70, 160, 100, COL_RED);
            for (a = 0; a < 1024u && a <= p; a += 16u)
                disc(120 + pol_x((int)a, 30), 112 + pol_y((int)a, 30), 3, COL_RED);
            text_c(120, 152, ui.mode ? "SILENCE..." : "PURGE LOOP...", 1, COL_WHITE);
        }
        draw_message();
    }
    for (y = 0; y < (EMB_H + 31) / 32; y++)
        row_dirty[y] = 0;
    for (y = 0; y < EMB_H; y++) {                   /* which rows changed: the FM-1 sends only those */
        const uint16_t *p = fb + y * EMB_W;
        uint32_t h = 2166136261u, x;
        for (x = 0; x < EMB_W; x++)
            h = (h ^ p[x]) * 16777619u;
        if (h != ui.row_hash[y]) {
            ui.row_hash[y] = h;
            row_dirty[y >> 5] |= 1u << (y & 31u);
            any = 1;
        }
    }
    return any;
}

/* ------------------------------------------------------------------ SysEx: the web preset console
 * Body (between F0 and F7): 7D 45 4D cmd ...  (7D: non-commercial id; "EM")
 *   01                  identity        -> 01 'E' 'M' 'B' ver
 *   02 s c              get chunk c (256 B) of slot s (0..15 scenes, 16 workbench)   -> 02 s c pack7(256)
 *   03 s c pack7(256)   put a chunk; the last one checks and stores the song         -> 03 s c status
 *   04 s                factory scene s                                              -> 04 s 0
 *   06 s                cue scene s (a game master's laptop)                         -> 06 s 0      */
#define SX_CHUNK 256u
#define SX_NCHUNK ((sizeof(emb_song_t) + SX_CHUNK - 1u) / SX_CHUNK)
static uint8_t sx_stage[SX_NCHUNK * SX_CHUNK];

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}
static uint32_t unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

uint32_t emb_sysex(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t s, c, o = 4;
    emb_song_t *song;
    if (n < 4u || in[0] != 0x7Du || in[1] != 0x45u || in[2] != 0x4Du)
        return 0;
    out[0] = 0x7D;
    out[1] = 0x45;
    out[2] = 0x4D;
    out[3] = in[3];
    s = n > 4u ? in[4] : 0u;
    c = n > 5u ? in[5] : 0u;
    song = s < EMB_NSLOT ? &emb_slot[s] : &emb_bench;
    switch (in[3]) {
    case 1:
        out[o++] = 'E';
        out[o++] = 'M';
        out[o++] = 'B';
        out[o++] = 1;
        out[o++] = (uint8_t)SX_NCHUNK;
        out[o++] = (uint8_t)(sizeof(emb_song_t) & 0x7Fu);   /* a song's bytes, two 7-bit halves */
        out[o++] = (uint8_t)(sizeof(emb_song_t) >> 7);
        return o;
    case 2:
        if (s > EMB_NSLOT || c >= SX_NCHUNK)
            return 0;
        out[o++] = (uint8_t)s;
        out[o++] = (uint8_t)c;
        {
            uint8_t buf[SX_CHUNK];
            uint32_t i, base = c * SX_CHUNK;
            seal_song(song);
            for (i = 0; i < SX_CHUNK; i++)
                buf[i] = base + i < sizeof *song ? ((const uint8_t *)song)[base + i] : 0;
            return o + pack7(buf, SX_CHUNK, out + o);
        }
    case 3: {
        uint32_t got;
        if (s > EMB_NSLOT || c >= SX_NCHUNK)
            return 0;
        got = unpack7(in + 6, n - 6u, sx_stage + c * SX_CHUNK, SX_CHUNK);
        out[o++] = (uint8_t)s;
        out[o++] = (uint8_t)c;
        if (got < SX_CHUNK) {
            out[o++] = 2;                           /* short chunk */
            return o;
        }
        if (c + 1u == SX_NCHUNK) {                  /* the last: check, then take it */
            const emb_song_t *in_song = (const emb_song_t *)(const void *)sx_stage;
            if (!song_ok(in_song)) {
                out[o++] = 1;
                return o;
            }
            *song = *in_song;
            if (s < EMB_NSLOT)
                ui.save_slots |= (uint16_t)(1u << s);
            else
                ui.save_due |= 1u;
            save_now();
        }
        out[o++] = 0;
        return o;
    }
    case 4:
        if (s >= EMB_NSLOT)
            return 0;
        emb_scene_factory(s, &emb_slot[s]);
        ui.save_slots |= (uint16_t)(1u << s);
        save_now();
        out[o++] = (uint8_t)s;
        out[o++] = 0;
        return o;
    case 6:
        if (s >= EMB_NSLOT)
            return 0;
        ui.last_scene = (uint8_t)s;
        emb_post(C_QUEUE, s, 0, 0, 0);
        out[o++] = (uint8_t)s;
        out[o++] = 0;
        return o;
    }
    return 0;
}
