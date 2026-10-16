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
 *   ENCOUNTER       white keys 1, 5, 9, 13 (the first of each row of four): the scene groups REST, EXPLORE,
 *                   COMBAT, CLIMAX, each cueing its first scene (crossfade on the next bar); the sharps after a
 *                   group key: its variations II, III, IV; a group key held 0.6 s: variation IV; the other white
 *                   keys: the battle modifiers. K1 intensity  K2 tempo  K3 Sanctuary  K4 music volume;
 *                   ARP duck; FX cathedral swell
 */

enum { P_HOME, P_FX, P_ENV, P_MOTION, P_VOICE, P_MODE, P_SEQ };
static const char *const PAGE_NAME[7] = {"HOME", "SANCTUARY", "ENVELOPE", "MOTION", "VOICE", "MODES", "STEPS"};
#define REL_BTNS ((1u << B_HOME) | (1u << B_SEL) | (1u << B_EDIT) | (1u << B_GLO) | (1u << B_SAVE))
#define TRACK_DRUM 3u

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
    uint8_t boot_started, batt, ext, vol_init, vol_ref;
    int8_t cat_key;                                 /* Encounter: the group key held (for variation IV), -1 none */
    uint32_t cat_t0, vol_t;                         /* vol_t: the MASTER card shows until then (0 none) */
    const char *boot_note;
} ui;

/* ---- Encounter: what each key does ---- */
static const int8_t CAT_KEY[4] = {0, 7, 14, 21};    /* the first white key of each row of four: F3, C4, G4, D5 */
static const int8_t MOD_OF_WHITE[16] = {-1, 0, 2, 3, -1, 4, 5, 1, -1, 6, 7, 8, -1, 9, 10, 11};
#define MOD_SWELL 11
/* the scene key k cues: a group key its group's first scene, the n-th sharp after it the group's n+1-th; -1 none */
static int enc_slot(uint32_t k)
{
    int c = 3, n = 0;
    uint32_t j;
    while (c > 0 && (int)k < CAT_KEY[c])
        c--;
    if ((int)k == CAT_KEY[c])
        return c * 4;
    if (emb_black_of[k] < 0)
        return -1;
    for (j = (uint32_t)CAT_KEY[c] + 1u; j <= k; j++)
        n += emb_black_of[j] >= 0;
    return n <= 3 ? c * 4 + n : -1;
}
static int enc_key_of_slot(uint32_t s)             /* the key that cues slot s (the group key for a held IV) */
{
    uint32_t k;
    for (k = 0; k < EMB_NKEY; k++)
        if (enc_slot(k) == (int)s)
            return (int)k;
    return CAT_KEY[(s >> 2) & 3u];
}

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
    ui.cat_key = -1;
    ui.batt = 255;                                  /* (until the platform says: shown as on USB) */
    ui.music_vol = 127;
    ui.tempo_pm = 1000;
    for (k = 0; k < EMB_NTRK; k++) {
        ui.organ_prev[k] = I_POSITIVE;
        ui.last_note[k] = (uint8_t)(k == TRACK_DRUM ? 1 : 48 + emb_bench.root);
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
    if (ui.trk == TRACK_DRUM) {
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
    if (ui.trk == TRACK_DRUM)                       /* drums: white keys the pieces, black a ghost of the left one */
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
        s->note[0] = ui.last_note[ui.trk] ? ui.last_note[ui.trk] : (uint8_t)(ui.trk == TRACK_DRUM ? 1 : 50);
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
        if (down && ui.boot_started && ui.now - ui.boot_t0 > 600u)
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
        if (ui.save_key >= 0 && ui.save_key == enc_slot(k))
            ui.save_key = -1;
        if (ui.cat_key == (int)k)
            ui.cat_key = -1;
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
    if (held(B_SAVE)) {                             /* SAVE + a scene's key: hold to store / to fetch */
        ui.used |= 1u << B_SAVE;
        if (enc_slot(k) >= 0) {
            ui.save_key = (int8_t)enc_slot(k);
            ui.save_t0 = ui.now;
        }
        return;
    }
    if (ui.mode) {                                  /* ---- Encounter ---- */
        int s = enc_slot(k);
        if (s >= 0) {
            ui.last_scene = (uint8_t)s;
            emb_post(C_QUEUE, (uint32_t)s, 0, 0, 0);
            say(emb_slot[s].name);
            if ((s & 3) == 0) {                     /* a group key: held, it cues variation IV */
                ui.cat_key = (int8_t)k;
                ui.cat_t0 = ui.now;
            }
        } else if (w >= 0 && MOD_OF_WHITE[w] >= 0) {
            int m = MOD_OF_WHITE[w];
            if (m == MOD_SWELL)
                emb_post(C_FX, 2, 0, 0, 0);
            else
                emb_post(C_MOD, (uint32_t)m, 0, 0, 0);
            say(emb_mod_name[m]);
        }
        (void)bl;
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
        emb_post(C_NOTE_ON, ui.trk, (uint32_t)n, bl >= 0 && ui.trk == TRACK_DRUM ? 60u : vel, 0);
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
            if (ui.trk == TRACK_DRUM)
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
            if (k == 3) emb_set.out_level = (uint8_t)clamp_add(emb_set.out_level, -d, 0, 2);   /* clockwise: louder */
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
            int lo = ui.trk == TRACK_DRUM ? 0 : I_FIRST_MELODIC, hi = ui.trk == TRACK_DRUM ? I_RITUAL_KIT : I_COUNT - 1;
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

/* MASTER: its level, and the volume card when it is turned (2 steps of 255 to show it, so the pot's own jitter
 * never does; the first reading after power-on is only taken) */
void emb_master(uint32_t q12)
{
    uint32_t v = q12 >> 4 > 255u ? 255u : q12 >> 4;
    int d = (int)v - (int)ui.vol_ref;
    emb_master_vol = (uint8_t)v;
    if (!ui.vol_init || ui.booting) {
        ui.vol_init = 1;
        ui.vol_ref = (uint8_t)v;
        return;
    }
    if (d >= 2 || d <= -2 || (v != ui.vol_ref && (v == 0u || v == 255u))) {
        ui.vol_ref = (uint8_t)v;
        ui.vol_t = (ui.now + 1000u) | 1u;
    }
}

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
        else if (ui.mode && d1 == 52u + MOD_SWELL)
            emb_post(C_FX, 2, 0, 0, 0);
        else if (ui.mode && d1 >= 52u && d1 < 52u + MOD_SWELL)
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
    if (!ui.boot_started) {                         /* the animation's clock starts at the first tick */
        ui.boot_started = 1;
        ui.boot_t0 = ms;
    }
    if (ui.booting && ms - ui.boot_t0 > BOOT_MS)
        ui.booting = 0;
    if (ui.cat_key >= 0 && ms - ui.cat_t0 >= 600u) {   /* a group key held: its variation IV */
        int s = enc_slot((uint32_t)ui.cat_key) + 3;
        ui.cat_key = -1;
        ui.last_scene = (uint8_t)s;
        emb_post(C_QUEUE, (uint32_t)s, 0, 0, 0);
        say(emb_slot[s].name);
    }
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
        uint32_t k, cur = emb_st.playing ? emb_st.deck_slot[emb_st.active] : 255u;
        uint32_t q = emb_st.queued && emb_st.queued != 255u ? emb_st.queued - 1u : 255u;
        int qk = q < 16u ? enc_key_of_slot(q) : -1;
        lv[B_HOME] = style == 2u ? LED_OFF : LED_MID;   /* the steady half light says "Encounter" */
        lv[B_ARP] = emb_st.duck ? LED_LIT : glow;
        lv[B_FX] = emb_st.swell ? LED_LIT : glow;
        for (k = 0; k < EMB_NKEY; k++) {
            int s = enc_slot(k), w = emb_white_of[k];
            uint8_t l = LED_OFF;
            if (s >= 0 && (s & 3) == 0)             /* a group key: lit while its group plays, else a landmark */
                l = cur < 16u && cur >> 2 == (uint32_t)s >> 2 ? LED_LIT : style == 2u ? LED_OFF : LED_MID;
            else if (s >= 0)                        /* a variation: lit while it plays */
                l = cur == (uint32_t)s ? LED_LIT : LED_OFF;
            else if (w >= 0 && MOD_OF_WHITE[w] >= 0) {   /* a battle modifier: dark, lit while in effect */
                switch (MOD_OF_WHITE[w]) {
                case 0: l = emb_st.mod_mode == M_PHRYGIAN; break;
                case 2: l = emb_st.mod_mode == M_HARMONIC; break;
                case 3: l = emb_st.mod_mode == M_IONIAN; break;
                case 4: l = (int8_t)emb_st.key_shift > 0; break;
                case 5: l = (int8_t)emb_st.key_shift < 0; break;
                case 6: l = emb_st.rate_t1 == 4u; break;
                case 7: l = emb_st.rate_t1 == 1u; break;
                case 8: l = (emb_st.hold >> 6) & 1u; break;
                case MOD_SWELL: l = emb_st.swell; break;
                }
                l = l ? LED_LIT : LED_OFF;
            }
            if ((int)k == qk)                       /* queued: blinks fast until the mark */
                l = fast ? LED_LIT : LED_OFF;
            lv[B_COUNT + k] = l;
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
        if (ui.trk == TRACK_DRUM)
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

/* ------------------------------------------------------------------ screens
 * 240 x 240: a top bar (0..17: the mode chip, the key or page, BPM, the battery), the content, a row of knob
 * chips (203..216, in the knobs' own colours), and the tips footer (222..239, paper, keycaps in ink). On top:
 * the toast, the MASTER volume card, the purge ring. */
#define FOOT_Y 222
#define CHIP_Y 203

void emb_power(uint32_t pct, uint32_t ext)
{
    ui.batt = (uint8_t)(pct > 255u ? 255u : pct);
    ui.ext = (uint8_t)(ext != 0u);
}
void emb_boot_note(const char *s) { ui.boot_note = s; }
void emb_redraw(void)
{
    uint32_t y;
    for (y = 0; y < EMB_H; y++)
        ui.row_hash[y] = 0x5EEDu ^ y;               /* (no frame hashes to these) */
}

static void trunc_cpy(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static void top_bar(const char *chip, uint16_t chip_c, const char *mid, const char *right)
{
    int w = text_w(chip, 1) + 12;
    fill(0, 0, EMB_W, 18, COL_INK);
    fill(0, 0, w, 18, chip_c);
    text(6, 6, chip, 1, COL_INK);
    if (mid)
        text(w + 6, 6, mid, 1, COL_PAPER);
    if (right)
        text(212 - text_w(right, 1) - 6, 6, right, 1, COL_PAPER);
    battery(214, 4, ui.batt, ui.ext || ui.batt == 255u);
    fill(0, 18, EMB_W, 2, COL_INK2);
}

static void knob_chips(const char *const lab[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        int x = 3 + k * 59;
        fill(x, CHIP_Y, 56, 14, KNOB_COL[k]);
        text_c(x + 28, CHIP_Y + 4, lab[k], 1, COL_INK);
    }
}

/* a tip: plain words, [KEY] drawn as a keycap; centred on the paper footer */
static int tip_w(const char *s)
{
    int w = 0;
    for (; *s; s++)
        w += *s == '[' ? 3 : *s == ']' ? 3 : 6;
    return w;
}
static void footer(const char *tip)
{
    int x = (EMB_W - tip_w(tip)) / 2, cap = 0;
    char ch[2] = {0, 0};
    fill(0, FOOT_Y, EMB_W, EMB_H - FOOT_Y, COL_PAPER);
    for (; *tip; tip++) {
        if (*tip == '[') {
            cap = 1;
            fill(x, FOOT_Y + 4, 3, 11, COL_INK);
            x += 3;
            continue;
        }
        if (*tip == ']') {
            cap = 0;
            fill(x - 1, FOOT_Y + 4, 4, 11, COL_INK);
            x += 3;
            continue;
        }
        if (cap)
            fill(x - 1, FOOT_Y + 4, 7, 11, COL_INK);
        ch[0] = *tip;
        text(x, FOOT_Y + 6, ch, 1, cap ? COL_PAPER : COL_INK);
        x += 6;
    }
}

static const char *const TIP_ENC[] = {
    "[HOME]+[PLAY] BARDIC MODE", "KEYS 1 5 9 13: SCENE GROUPS", "SHARPS: VARIATIONS II-IV",
    "HOLD A GROUP KEY: VARIATION IV", "OTHER WHITES: BATTLE MODS", "HOLD [HOME]+[REC]: SILENCE",
    "[ARP] DUCK FOR A SPEECH", "[FX] CATHEDRAL SWELL", "[SELECT] CROSSFADE LENGTH"};
static const char *const TIP_HOME[] = {
    "[HOME]+[PLAY] ENCOUNTER", "[SEL] NEXT TRACK", "[PRESETS] INSTRUMENT", "[REC] LIVE LOOP",
    "HOLD [HOME]+[REC]: PURGE", "HOLD [SAVE]: KEEP THE SONG", "[EDIT]+[GLO] TONEWHEEL", "[ARP] HOLD A DRONE",
    "[ALGORITHM] TEMPO"};
static const char *const TIP_SEQ[] = {
    "TAP A KEY: SET A STEP", "HOLD KEY + [K1]: NOTE", "HOLD KEY + [K2]: TIMING", "HOLD KEY + [K4]: TONE LOCK",
    "[OP1]-[OP4] STEP PAGES", "[SEQ] OR [HOME]: BACK"};
static const char *const TIP_PAGE[] = {"[K1]-[K4] SHAPE THE SOUND", "[HOME] BACK", "[SEL] NEXT TRACK"};
#define NTIPS(a) (sizeof(a) / sizeof(a[0]))

static const char *tip_now(void)
{
    uint32_t r = emb_udiv(ui.now, 3200u);
    if (ui.mode)
        return TIP_ENC[r % NTIPS(TIP_ENC)];
    if (ui.page == P_HOME)
        return TIP_HOME[r % NTIPS(TIP_HOME)];
    if (ui.page == P_SEQ)
        return TIP_SEQ[r % NTIPS(TIP_SEQ)];
    if (ui.page == P_MOTION && cur_trk()->inst == I_TONEWHEEL && (r & 1u))
        return "[LFO] AGAIN: LESLIE FAST/SLOW";
    if (ui.page == P_MODE)
        return (r & 1u) ? "[GLO] AGAIN: THE BOX PAGE" : "[K4] SNAP KEYS TO THE MODE";
    return TIP_PAGE[r % NTIPS(TIP_PAGE)];
}

static const char *const ROMAN[4] = {"I", "II", "III", "IV"};
static const char *const FAMILY[4] = {"REST", "EXPLORE", "COMBAT", "CLIMAX"};

static void draw_encounter(void)
{
    char b[32];
    uint32_t r, c, cur = emb_st.playing ? emb_st.deck_slot[emb_st.active] : 255u;
    uint32_t q = emb_st.queued && emb_st.queued != 255u ? emb_st.queued - 1u : 255u, blink = (ui.now >> 7) & 1u;
    static const char *const LAB[4] = {"INTENS", "TEMPO", "HALL", "VOLUME"};
    b[0] = 0;
    num(b, (int32_t)emb_udiv((cur < 16u ? emb_slot[cur].bpm : 96u) * (uint32_t)ui.tempo_pm, 1000u));
    cat(b, " BPM");
    top_bar("ENCOUNTER", COL_BLUE, 0, b);
    if (cur < 16u) {                                /* now playing */
        uint32_t fam = cur >> 2;
        card(4, 23, 229, 42, FAMILY_COL[fam], COL_INK);
        sprite(10, 29, fam, 3, COL_INK);
        text(46, 28, emb_slot[cur].name, 2, COL_INK);
        b[0] = 0;
        cat(b, FAMILY[fam]);
        cat(b, " ");
        cat(b, ROMAN[cur & 3u]);
        text(46, 47, b, 1, COL_INK);
    } else {
        card(4, 23, 229, 42, COL_INK2, COL_GREY);
        text_c(118, 33, "SILENCE", 2, COL_GREY);
        text_c(118, 51, "PRESS 1, 5, 9 OR 13", 1, COL_GREY);
    }
    if (q < 16u) {                                  /* what comes on the mark */
        char n[6];
        b[0] = 0;
        cat(b, "NEXT ");
        num(n, (int32_t)(emb_st.bar_left ? emb_st.bar_left : 16u));
        cat(b, n);
        fill(160, 55, 70, 9, COL_INK);
        text(164, 56, b, 1, blink ? COL_PAPER : COL_YELLOW);
    }
    {   /* the keys as they lie: a row per group, the group key, then its sharps */
        static const char *const HEAD[4] = {"KEY", "#1", "#2", "#3"};
        for (c = 0; c < 4u; c++)
            text_c(53 + (int)c * 53, 70, HEAD[c], 1, COL_GREY);
    }
    for (r = 0; r < 4u; r++) {
        int y = 80 + (int)r * 27;
        fill(3, y, 22, 24, FAMILY_COL[r]);
        sprite(5, y + 3, r, 2, COL_INK);
        for (c = 0; c < 4u; c++) {
            uint32_t s = r * 4u + c;
            int x = 28 + (int)c * 53;
            uint16_t bg = COL_INK2, fg = FAMILY_COL[r];
            char t[8];
            if (s == cur) {
                bg = FAMILY_COL[r];
                fg = COL_INK;
            }
            if (s == q && blink) {
                bg = COL_PAPER;
                fg = COL_INK;
            }
            fill(x, y, 50, 24, bg);
            if (bg == COL_INK2)
                fill(x, y, 2, 24, FAMILY_COL[r]);
            trunc_cpy(t, emb_slot[s].name, 7);
            text_c(x + 26, y + 9, t, 1, fg);
        }
    }
    {   /* the battle's state: intensity pips, the bends in effect */
        int x = 4;
        for (c = 0; c < 4u; c++)
            fill(x + (int)c * 9, 190, 7, 9, c <= emb_st.intensity ? COL_PAPER : COL_INK2);
        x = 46;
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
        if (emb_st.duck)
            cat(b, " DUCK");
        if (emb_st.swell)
            cat(b, " SWELL");
        text(x, 191, b[0] ? b : "MOTHER MELODY HEARS", 1, b[0] ? COL_ORANGE : COL_GREY);
    }
    knob_chips(LAB);
}

static void draw_bardic_home(void)
{
    char b[32], mid[32];
    uint32_t t, i, live = emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u;
    static const char *const LAB[4] = {"LVL 1", "LVL 2", "LVL 3", "LVL 4"};
    {
        char n[8];
        note_name(n, emb_bench.root);
        n[n[1] == '#' ? 2 : 1] = 0;
        mid[0] = 0;
        cat(mid, n);
        cat(mid, " ");
        cat(mid, emb_mode_name[emb_bench.mode]);
        if ((emb_st.hold >> 7) & 1u)
            cat(mid, " REC");
    }
    b[0] = 0;
    num(b, emb_bench.bpm);
    cat(b, " BPM");
    top_bar("BARDIC", COL_ORANGE, mid, b);
    for (t = 0; t < EMB_NTRK; t++) {
        const emb_track_t *tr = &emb_bench.t[t];
        int y = 22 + (int)t * 44, muted = (emb_mutes >> t) & 1u;
        uint32_t ph = emb_st.step[emb_st.active][t], page = live ? ph >> 4 : 0u;
        uint16_t tc = muted ? COL_GREY : TRACK_COL[t];
        if (t == ui.trk)
            fill(0, y - 1, EMB_W, 42, COL_INK2);
        fill(3, y, 28, 40, tc);
        b[0] = (char)('1' + t);
        b[1] = 0;
        text(10, y + 13, b, 3, COL_INK);
        text(36, y + 3, emb_inst_name[tr->inst], 1, t == ui.trk ? COL_PAPER : COL_LIGHT);
        if (muted)
            text(200, y + 3, "MUTE", 1, COL_GREY);
        else if ((emb_st.hold >> t) & 1u)
            text(200, y + 3, "HOLD", 1, COL_YELLOW);
        for (i = 0; i < 16u; i++) {
            uint32_t idx = page * 16u + i;
            const emb_step_t *s = &tr->s[idx];
            int x = 36 + (int)i * 12;
            uint16_t c = idx >= tr->len ? COL_INK : (s->note[0] || s->note[1] || s->note[2] || s->note[3]) ? tc : COL_GREY;
            if (live && idx == ph)
                c = COL_PAPER;
            fill(x, y + 15 + ((i & 3u) ? 2 : 0), 10, (i & 3u) ? 18 : 22, c);   /* beats stand taller */
        }
        if (t == ui.trk)
            fill(0, y - 1, 2, 42, COL_PAPER);
    }
    if (ui.oct) {
        b[0] = 'O';
        b[1] = 'C';
        b[2] = 'T';
        b[3] = ' ';
        num(b + 4, ui.oct);
        text(EMB_W - 48, 196, b, 1, COL_GREY);
    }
    knob_chips(LAB);
}

static void draw_seq(void)
{
    const emb_track_t *t = cur_trk();
    char b[32];
    uint32_t i, ph = emb_st.step[emb_st.active][ui.trk], live = emb_st.playing && emb_st.deck_slot[emb_st.active] == 255u;
    static const char *const LAB[4] = {"LENGTH", "SWING", "LEVEL", "REVERB"};
    static const char *const LAB_H[4] = {"NOTE", "TIMING", "VELO", "LOCK"};
    uint16_t tc = TRACK_COL[ui.trk];
    b[0] = 'T';
    b[1] = (char)('1' + ui.trk);
    b[2] = ' ';
    b[3] = 0;
    {
        char n[4];
        num(n, ui.seq_page + 1);
        cat(b, n);
    }
    cat(b, "/4");
    if (ui.step_held >= 0) {
        const emb_step_t *s = &t->s[ui.step_held];
        char n[8];
        b[0] = 'V';
        b[1] = 0;
        num(n, s->vel);
        cat(b, n);
        cat(b, " G");
        num(n, s->gate);
        cat(b, n);
        cat(b, " N");
        num(n, s->micro);
        cat(b, n);
    }
    top_bar("STEPS", tc, ui.step_held >= 0 ? 0 : emb_inst_name[t->inst], b);
    for (i = 0; i < 16u; i++) {
        uint32_t idx = ui.seq_page * 16u + i;
        const emb_step_t *s = &t->s[idx];
        int x = 3 + (int)(i & 3u) * 59, y = 23 + (int)(i >> 2) * 44;
        int on = s->note[0] != 0, held_here = (int)idx == ui.step_held;
        uint16_t bg = idx >= t->len ? COL_INK : on ? tc : COL_INK2;
        if (held_here)
            bg = COL_PAPER;
        fill(x, y, 55, 40, bg);
        if (live && idx == ph) {
            fill(x, y, 55, 3, COL_PAPER);
            fill(x, y + 37, 55, 3, COL_PAPER);
        }
        if (on) {
            char n[8];
            if (ui.trk == TRACK_DRUM)
                trunc_cpy(n, emb_drum_name[(s->note[0] - 1u) & 7u], 4);
            else
                note_name(n, s->note[0]);
            text(x + 5, y + 8, n, n[3] ? 1 : 2, COL_INK);
            if (s->note[1])
                text(x + 46, y + 4, "+", 1, COL_INK);
            if (s->micro)
                fill(x + 26 + s->micro, y + 30, 3, 5, COL_INK);   /* the nudge */
            if (s->lock != 255u)
                fill(x + 46, y + 30, 5, 5, COL_INK);
        } else if (idx < t->len) {
            char n[4];
            num(n, (int32_t)idx + 1);
            text(x + 5, y + 5, n, 1, COL_GREY);
        }
    }
    knob_chips(ui.step_held >= 0 ? LAB_H : LAB);
}

static void draw_page(void)
{
    emb_track_t *t = cur_trk();
    const char *lab[4];
    char val[4][12], title[16];
    int v[4], vmax[4], k;
    for (k = 0; k < 4; k++) {
        lab[k] = "";
        v[k] = 0;
        vmax[k] = 127;
    }
    switch (ui.page) {
    case P_FX:
        lab[0] = "SIZE"; lab[1] = "DECAY"; lab[2] = "DAMP"; lab[3] = "MIX";
        v[0] = emb_set.rv_size; v[1] = emb_set.rv_decay; v[2] = emb_set.rv_damp; v[3] = emb_set.rv_mix;
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
            lab[0] = "A4 CAL"; lab[1] = "MIDI CH"; lab[2] = "LEDS"; lab[3] = "OUTPUT";
            v[0] = emb_set.tune + 50; vmax[0] = 100; v[1] = emb_set.midi_ch; vmax[1] = 15;
            v[2] = emb_set.leds; vmax[2] = 2; v[3] = 2 - emb_set.out_level; vmax[3] = 2;
        }
        break;
    }
    for (k = 0; k < 4; k++)
        num(val[k], v[k]);
    if (ui.page == P_MODE && ui.sub == 0u) {        /* names instead of numbers */
        static const char *const MS[M_COUNT] = {"ION", "DOR", "PHR", "LYD", "MIX", "AEO", "HARM", "PDOM"};
        static const char *const TS[T_COUNT] = {"EQ", "PYTH", "MEAN", "JUST"};
        note_name(val[0], emb_bench.root);
        val[0][val[0][1] == '#' ? 2 : 1] = 0;
        trunc_cpy(val[1], MS[emb_bench.mode], 11);
        trunc_cpy(val[2], TS[emb_bench.temper], 11);
        trunc_cpy(val[3], emb_set.snap ? "ON" : "OFF", 11);
    }
    if (ui.page == P_MODE && ui.sub == 1u) {
        static const char *const LS[3] = {"GLOW", "LIT", "DARK"};
        static const char *const OS[3] = {"LOUD", "NORM", "SAFE"};
        num(val[0], emb_set.tune);
        num(val[1], emb_set.midi_ch + 1);
        trunc_cpy(val[2], LS[emb_set.leds % 3u], 11);
        trunc_cpy(val[3], OS[emb_set.out_level % 3u], 11);
    }
    title[0] = 'T';
    title[1] = (char)('1' + ui.trk);
    title[2] = ' ';
    trunc_cpy(title + 3, emb_inst_name[t->inst], 11);
    top_bar(ui.page == P_MODE && ui.sub ? "THE BOX" : PAGE_NAME[ui.page], COL_ORANGE,
            ui.page == P_FX || ui.page == P_MODE ? 0 : title, 0);
    for (k = 0; k < 4; k++) {                       /* a card per knob, in its colour */
        int x = 4 + (k & 1) * 118, y = 24 + (k >> 1) * 88, bar = (int)emb_udiv((uint32_t)(v[k] * 90), (uint32_t)vmax[k]);
        char kn[3] = {'K', (char)('1' + k), 0};
        card(x, y, 110, 80, KNOB_COL[k], COL_INK);
        text(x + 8, y + 8, lab[k], 1, COL_INK);
        text(x + 90, y + 8, kn, 1, COL_INK);
        text(x + 8, y + 26, val[k], text_w(val[k], 1) > 24 ? 2 : 3, COL_INK);   /* up to 4 characters large */
        fill(x + 8, y + 60, 94, 10, COL_INK);
        fill(x + 10, y + 62, bar, 6, KNOB_COL[k] == COL_LIGHT ? COL_ORANGE : COL_PAPER);
    }
    if (ui.page == P_MODE && ui.sub == 0u) {
        char b[40];
        b[0] = 0;
        cat(b, emb_mode_name[emb_bench.mode]);
        cat(b, " / ");
        cat(b, emb_temper_name[emb_bench.temper]);
        text_c(120, 204, b, 1, COL_PAPER);
    } else if (ui.page == P_MODE)
        text_c(120, 204, "EVANDER'S BOX V" EMB_VERSION, 1, COL_GREY);
    else if (ui.page == P_MOTION && t->inst == I_TONEWHEEL)
        text_c(120, 204, emb_st.leslie ? "LESLIE: FAST" : "LESLIE: SLOW", 1, COL_PAPER);
}

static void draw_toast(void)
{
    int big;
    uint32_t n = 0;
    if (!ui.msg[0] || (int32_t)(ui.now - ui.msg_t) > 0)
        return;
    while (ui.msg[n])
        n++;
    big = n <= 17u;
    card(10, 98, 217, big ? 34 : 26, COL_PAPER, COL_INK);
    text_c(118, big ? 108 : 107, ui.msg, big ? 2 : 1, COL_INK);
}

static void draw_volume(void)                       /* the MASTER card: up while the knob turns, a second after */
{
    uint32_t pct = emb_udiv((uint32_t)emb_master_vol * 100u + 127u, 255u), seg, k;
    char b[8];
    if (!ui.vol_t || (int32_t)(ui.now - ui.vol_t) > 0)
        return;
    card(24, 74, 189, 82, COL_PAPER, COL_INK);
    sprite(36, 86, SP_SPEAKER, 2, COL_INK);
    text(62, 89, "VOLUME", 2, COL_INK);
    num(b, (int32_t)pct);
    cat(b, "%");
    text(36, 110, b, 3, COL_INK);
    seg = emb_udiv(pct * 12u + 50u, 100u);
    for (k = 0; k < 12u; k++)
        fill(36 + (int)k * 14, 138, 11, 10, k < seg ? (k >= 10u ? COL_PINK : COL_ORANGE) : COL_LIGHT);
}

uint32_t emb_draw(uint16_t *fb, uint32_t row_dirty[(EMB_H + 31) / 32])
{
    uint32_t y, any = 0;
    G = fb;
    if (ui.booting) {
        boot_frame(ui.boot_started ? ui.now - ui.boot_t0 : 0u, ui.ext ? ui.boot_note : 0);
    } else {
        fill(0, 0, EMB_W, EMB_H, COL_INK);
        if (ui.mode)
            draw_encounter();
        else if (ui.page == P_HOME)
            draw_bardic_home();
        else if (ui.page == P_SEQ)
            draw_seq();
        else
            draw_page();
        footer(tip_now());
        if (held(B_HOME) && held(B_REC) && !ui.panic_fired) {   /* the purge ring fills while held */
            uint32_t p = emb_udiv((ui.now - ui.panic_t0) * 1024u, 400u), a;
            card(40, 66, 157, 100, COL_PINK, COL_INK);
            for (a = 0; a < 1024u && a <= p; a += 32u)
                fill(116 + pol_x((int)a, 26), 104 + pol_y((int)a, 26), 6, 6, COL_INK);
            text_c(118, 144, ui.mode ? "SILENCE..." : "PURGE LOOP...", 1, COL_INK);
        }
        draw_volume();
        draw_toast();
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
