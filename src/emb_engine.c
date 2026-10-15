/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: the engine. Two decks (what is playing, what fades in), the clock, the step
 * sequencer with microtiming and parameter locks, live looping, the Encounter modifiers, the mixer.
 * Audio side: everything here runs inside emb_render, fed by the command ring. */

const uint8_t emb_scale[M_COUNT][7] = {
    {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11},
    {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 1, 4, 5, 7, 8, 10}};
const char *const emb_mode_name[M_COUNT] = {"IONIAN", "DORIAN", "PHRYGIAN", "LYDIAN",
                                            "MIXOLYDIAN", "AEOLIAN", "HARM MINOR", "PHRYG DOM"};
const char *const emb_temper_name[T_COUNT] = {"EQUAL", "PYTHAGOREAN", "MEANTONE", "JUST"};
/* cents x 10 from equal temperament, by interval above the playing root */
static const int16_t TEMPER[T_COUNT][12] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, -98, 39, -59, 78, -20, 117, 20, -78, 59, -39, 98},        /* Pythagorean: pure fifths */
    {0, -240, -68, 103, -137, 34, -205, -34, -274, -103, 68, -171},   /* quarter-comma meantone */
    {0, 117, 39, 156, -137, -20, -98, 20, 137, -156, 176, -117},  /* 5-limit just */
};

emb_status_t emb_st;
emb_song_t emb_bench EMB_POOL;
emb_song_t emb_slot[EMB_NSLOT] EMB_POOL;
emb_settings_t emb_set;
uint8_t emb_master_vol = 200;
volatile uint8_t emb_mutes;                         /* Bardic track mutes (UI writes, audio reads) */

/* ---- the command ring: UI side writes w, audio side writes r ---- */
#define CQ 64u
static emb_cmd_t cq[CQ];
static volatile uint32_t cq_w, cq_r;
#ifndef EMB_BARRIER
#define EMB_BARRIER() __asm__ volatile("" ::: "memory")
#endif
void emb_post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t v)
{
    emb_cmd_t *e;
    if (cq_w - cq_r >= CQ)
        return;                                     /* full: a control gesture is lost, never audio */
    e = &cq[cq_w & (CQ - 1u)];
    e->op = (uint8_t)op;
    e->a = (uint8_t)a;
    e->b = (uint8_t)b;
    e->c = (uint8_t)c;
    e->v = v;
    EMB_BARRIER();
    cq_w++;
}

/* ------------------------------------------------------------------ pitch & scales */
int emb_in_scale(int note, uint32_t root, uint32_t mode)
{
    uint32_t pc = (uint32_t)(note - (int)root + 120) % 12u, d;
    for (d = 0; d < 7u; d++)
        if (emb_scale[mode][d] == pc)
            return 1;
    return 0;
}
int emb_snap(int note, uint32_t root, uint32_t mode)
{
    while (!emb_in_scale(note, root, mode))
        note--;
    return note;
}
/* a note of a song in (r0, m0), heard in (r1, m1): the same scale degree, the same chromatic offset */
static int remap(int note, uint32_t r0, uint32_t m0, int r1, uint32_t m1)
{
    uint32_t rel = (uint32_t)(note - (int)r0 + 120), o = rel / 12u, pc = rel % 12u, d = 6;
    if ((int)r0 == r1 && m0 == m1)
        return note;
    while (d && emb_scale[m0][d] > pc)
        d--;
    return r1 + (int)o * 12 - 120 + emb_scale[m1][d] + (int)(pc - emb_scale[m0][d]);
}
static int32_t temper_cents(int note, int root, uint32_t temper)
{
    return TEMPER[temper < T_COUNT ? temper : 0][(uint32_t)(note - root + 120) % 12u] / 10;
}

/* ------------------------------------------------------------------ decks */
typedef struct {
    emb_song_t *song;
    uint8_t on, slot, stopping;                     /* slot: 0..15, 255 workbench */
    int32_t gain, gain_t, gain_d;                   /* Q15, the crossfade ramp per block */
    int32_t tg[EMB_NTRK];                           /* track gains (intensity, mutes), Q15 */
    uint8_t ts[EMB_NTRK], tw[EMB_NTRK], tacc[EMB_NTRK];
    uint32_t gstep, gtw;
    int8_t held[EMB_NTRK][EMB_NNOTE];               /* voices held until the track's next note (gate 255) */
} deck_t;
static deck_t deck[2];
static uint32_t act;                                /* the deck in front */
static uint32_t spt_q16 = 1u << 24, tick_acc, cur_bpm, coup_n;
static uint8_t playing, queued, pend_mods_n;        /* queued: 0 none, else slot + 1, 255 + 1 -> 0x100 */
static uint16_t queue_slot = 0xFFFFu;
static uint8_t pend_mods[16];
static int8_t mode_over = -1, key_shift;
static uint8_t rate_t1 = 2, breakdown, intensity = 3, duck, swell, stinger_pending;
static int32_t music_gain = 32767, music_gain_t = 32767, tempo_pm = 1000;
static uint8_t rec_on, rec_track, hold_mask, leslie_fast;
static emb_track_t undo_track;
static uint8_t undo_valid, undo_trk;
static uint32_t cur_root_key = 0xFFFFu;             /* sympathetic strings' tuning */
static emb_track_t stinger_trk;

static int32_t bus[EMB_NTRK][EMB_CTL], les_bus[EMB_NTRK][EMB_CTL];
static int32_t mix_l[EMB_CTL], mix_r[EMB_CTL], send_l[EMB_CTL], send_r[EMB_CTL];

static int deck_root(const deck_t *d) { return (int)d->song->root + key_shift; }
static uint32_t deck_mode(const deck_t *d) { return mode_over >= 0 ? (uint32_t)mode_over : d->song->mode; }

static void voices_release(int deck_id, int trk, int fast)   /* -1: any */
{
    uint32_t i;
    for (i = 0; i < EMB_NVOICE; i++) {
        voice_t *v = &vox[i];
        if (!v->on || (deck_id >= 0 && v->deck != deck_id) || (trk >= 0 && v->trk != trk))
            continue;
        voice_release(v);
        if (fast)
            v->rk = 52000u;                         /* ~10 ms: no click, no tail */
    }
}

void emb_shed(void)                                 /* the oldest released voice goes, else the oldest */
{
    uint32_t i;
    voice_t *best = 0;
    for (i = 0; i < EMB_NVOICE; i++)
        if (vox[i].on && vox[i].stage == 3u && (!best || vox[i].age < best->age))
            best = &vox[i];
    if (!best)
        for (i = 0; i < EMB_NVOICE; i++)
            if (vox[i].on && (!best || vox[i].age < best->age))
                best = &vox[i];
    if (best) {
        voice_release(best);
        best->rk = 40000u;                          /* ~3 ms */
    }
}

static voice_t *voice_alloc(void)
{
    uint32_t i;
    voice_t *best = 0;
    for (i = 0; i < EMB_NVOICE; i++)
        if (!vox[i].on)
            return &vox[i];
    for (i = 0; i < EMB_NVOICE; i++)                /* steal: the oldest released, else the oldest */
        if (vox[i].stage == 3u && (!best || vox[i].age < best->age))
            best = &vox[i];
    if (!best)
        for (i = 0; i < EMB_NVOICE; i++)
            if (!best || vox[i].age < best->age)
                best = &vox[i];
    return best;
}

/* play one note: deck_id 0/1 (sequenced) or 2 (live / stinger); returns the voice index or -1 */
static int play_note(uint32_t deck_id, uint32_t trk, const emb_track_t *t, int note, uint32_t vel,
                     uint32_t tone, int root, uint32_t temper, uint32_t gate_samples)
{
    voice_t *v = voice_alloc();
    uint32_t inst = t->inst;
    int32_t cents = emb_set.tune;
    if (!v)
        return -1;
    if (inst < I_FIRST_MELODIC) {                   /* drums: note is the piece + 1 */
        uint32_t piece = (uint32_t)(note - 1) & 7u;
        v->piece = (uint8_t)piece;
        note = root + 36 + (piece == DR_GONG ? -12 : 0);   /* timpani and gong sound the root */
        while (note < 30)
            note += 12;
    } else {
        cents += temper_cents(note, root, temper);
    }
    voice_start(v, t, inst, (uint32_t)clampi(note, 0, 127), cents, (int32_t)vel * 258, tone);
    v->trk = (uint8_t)trk;
    v->deck = (uint8_t)deck_id;
    v->gate = gate_samples ? (int32_t)gate_samples : -1;
    return (int)(v - vox);
}

static uint32_t step_samples(void) { return (spt_q16 >> 16) * EMB_TPS; }

static void fire_step(uint32_t d_id, uint32_t trk)
{
    deck_t *d = &deck[d_id];
    emb_track_t *t = &d->song->t[trk];
    emb_step_t *s = &t->s[d->ts[trk]];
    uint32_t k, gate;
    int root = deck_root(d);
    if (!s->note[0] && !s->note[1] && !s->note[2] && !s->note[3])
        return;
    for (k = 0; k < EMB_NNOTE; k++)                 /* a new note ends the held ones */
        if (d->held[trk][k] >= 0) {
            voice_t *v = &vox[d->held[trk][k]];
            if (v->on && v->deck == d_id && v->trk == trk)
                voice_release(v);
            d->held[trk][k] = -1;
        }
    gate = s->gate == 255u ? 0u : ((uint32_t)s->gate * step_samples()) >> 2;
    for (k = 0; k < EMB_NNOTE; k++) {
        int n = s->note[k], vi;
        if (!n)
            continue;
        if (t->inst >= I_FIRST_MELODIC)
            n = remap(n, d->song->root, d->song->mode, root, deck_mode(d));
        vi = play_note(d_id, trk, t, n, s->vel ? s->vel : 100u, s->lock != 255u ? s->lock : t->tone, root,
                       d->song->temper, gate ? gate : 0u);
        if (s->gate == 255u)
            d->held[trk][k] = (int8_t)vi;
    }
}

static void deck_start(uint32_t d_id, emb_song_t *s, uint32_t slot, int32_t fade_blocks)
{
    deck_t *d = &deck[d_id];
    uint32_t k, j;
    d->song = s;
    d->slot = (uint8_t)slot;
    d->on = 1;
    d->stopping = 0;
    d->gstep = 0;
    d->gtw = 0;
    for (k = 0; k < EMB_NTRK; k++) {
        d->ts[k] = 0;
        d->tw[k] = 11;                              /* the first tick lands on the step (tw 12) */
        d->tacc[k] = 0;
        d->tg[k] = 32767;
        for (j = 0; j < EMB_NNOTE; j++)
            d->held[k][j] = -1;
        if (s->t[k].s[0].micro < 0)                 /* what comes early on step 0 sounds now */
            fire_step(d_id, k);
    }
    d->gain_t = 32767;
    d->gain = fade_blocks > 0 ? 0 : 32767;
    d->gain_d = fade_blocks > 0 ? clampi((int32_t)emb_udiv(32767u, (uint32_t)fade_blocks), 1, 32767) : 32767;
}

static void deck_stop(uint32_t d_id, int32_t fade_blocks)
{
    deck_t *d = &deck[d_id];
    if (!d->on)
        return;
    if (fade_blocks <= 0) {
        d->on = 0;
        voices_release((int)d_id, -1, 0);
        return;
    }
    d->stopping = 1;
    d->gain_t = 0;
    d->gain_d = clampi((int32_t)emb_udiv(32767u, (uint32_t)fade_blocks), 1, 32767);
}

static int32_t fade_blocks(void)                    /* the crossfade: settings' beats at this tempo */
{
    return (int32_t)emb_udiv(emb_set.fade_beats * 4u * step_samples(), EMB_CTL);
}

static emb_song_t *song_of(uint32_t slot) { return slot == 255u ? &emb_bench : &emb_slot[slot & 15u]; }

static void launch(uint32_t slot)                   /* at a boundary: the queued song takes the front */
{
    uint32_t back = act ^ 1u;
    int32_t fb = deck[act].on ? fade_blocks() : 0;
    if (deck[back].on)
        deck_stop(back, 0);
    deck_stop(act, fb);
    act = back;
    deck_start(act, song_of(slot), slot, fb);
    mode_over = -1;                                 /* a new scene brings its own key */
    key_shift = 0;
    rate_t1 = 2;
    breakdown = 0;
    playing = 1;
}

static void apply_mod(uint32_t m)
{
    switch (m) {
    case 0: mode_over = M_PHRYGIAN; break;
    case 1: mode_over = -1; key_shift = 0; rate_t1 = 2; breakdown = 0; break;
    case 2: mode_over = M_HARMONIC; break;
    case 3: mode_over = M_IONIAN; break;
    case 4: if (key_shift < 6) key_shift += 2; break;
    case 5: if (key_shift > -6) key_shift -= 2; break;
    case 6: rate_t1 = rate_t1 == 4u ? 2 : 4; break;
    case 7: rate_t1 = rate_t1 == 1u ? 2 : 1; break;
    case 8: breakdown ^= 1u; break;
    case 9: intensity = 3; breakdown = 0; break;
    case 10: stinger_pending = 1; break;
    }
}

static void stinger(void)                           /* a gong and a brass chord on the beat */
{
    const deck_t *d = &deck[act];
    int root = d->song ? deck_root(d) : 0;
    uint32_t temper = d->song ? d->song->temper : 0;
    emb_track_defaults(&stinger_trk, I_WAR_KIT);
    play_note(2, 0, &stinger_trk, DR_GONG + 1, 120, 64, root, temper, 0);
    play_note(2, 0, &stinger_trk, DR_WAR + 1, 127, 64, root, temper, 0);
    emb_track_defaults(&stinger_trk, I_BRASS);
    stinger_trk.att = 4;
    stinger_trk.rel = 60;
    play_note(2, 3, &stinger_trk, root + 48, 110, 90, root, temper, step_samples() * 3u);
    play_note(2, 3, &stinger_trk, root + 55, 100, 90, root, temper, step_samples() * 3u);
    play_note(2, 3, &stinger_trk, root + 60, 100, 90, root, temper, step_samples() * 3u);
}

/* one clock tick (1/24 step) for one deck */
static void deck_tick(uint32_t d_id)
{
    deck_t *d = &deck[d_id];
    uint32_t k;
    if (++d->gtw == EMB_TPS) {
        d->gtw = 0;
        d->gstep++;
    }
    for (k = 0; k < EMB_NTRK; k++) {
        uint32_t rate = k == 0u && d_id == act ? rate_t1 : 2u;   /* halves of a tick: 1 half, 2 normal, 4 double */
        d->tacc[k] = (uint8_t)(d->tacc[k] + rate);
        while (d->tacc[k] >= 2u) {
            const emb_track_t *t = &d->song->t[k];
            const emb_step_t *s;
            int32_t micro;
            d->tacc[k] = (uint8_t)(d->tacc[k] - 2u);
            if (++d->tw[k] == EMB_TPS) {
                d->tw[k] = 0;
                if (++d->ts[k] >= (t->len ? t->len : 16u))
                    d->ts[k] = 0;
            }
            s = &t->s[d->ts[k]];
            micro = s->micro + ((d->ts[k] & 1u) ? (int32_t)d->song->swing : 0);
            if (micro > 11)
                micro = 11;
            if ((int32_t)d->tw[k] - 12 == micro)
                fire_step(d_id, k);
        }
    }
}

/* the global clock: ticks, beats and the quantized switch */
static void clock_block(void)
{
    deck_t *a = &deck[act];
    uint32_t bpm = a->on && a->song ? a->song->bpm : emb_bench.bpm;
    bpm = (bpm * (uint32_t)tempo_pm) / 1000u;       /* (by a constant) */
    if (bpm < 30u)
        bpm = 30u;
    if (bpm != cur_bpm) {
        cur_bpm = bpm;
        spt_q16 = emb_udiv(1806336000u, bpm);       /* FS * 60 * 65536 / (4 * 24 * bpm) */
    }
    vib_ph += vib_inc;
    coup_n += EMB_CTL;
    coup_ph = coup_n << 16;
    if (!playing)
        return;
    tick_acc += EMB_CTL << 16;
    while (tick_acc >= spt_q16) {
        uint32_t qmask;
        tick_acc -= spt_q16;
        if (deck[act ^ 1u].on)
            deck_tick(act ^ 1u);
        if (!a->on)
            continue;
        deck_tick(act);
        if (a->gtw != 0u)
            continue;
        /* a step boundary of the front deck */
        if ((a->gstep & 1u) == 0u)
            coup_n = 0;                             /* an eighth: the hurdy-gurdy's coup */
        if ((a->gstep & 3u) == 0u) {                /* a beat */
            uint32_t k;
            emb_st.beats++;
            for (k = 0; k < pend_mods_n; k++)
                apply_mod(pend_mods[k]);
            pend_mods_n = 0;
            if (stinger_pending) {
                stinger_pending = 0;
                stinger();
            }
        }
        qmask = emb_set.quant == 0u ? 3u : emb_set.quant == 1u ? 15u : emb_set.quant == 2u ? 31u : 63u;   /* beat .. 4 bars */
        emb_st.bar_left = (uint8_t)((qmask + 1u - (a->gstep & qmask)) & qmask);
        if (queue_slot != 0xFFFFu && (a->gstep & qmask) == 0u) {
            uint32_t s = queue_slot;
            queue_slot = 0xFFFFu;
            launch(s);
            a = &deck[act];
        }
    }
}

/* ------------------------------------------------------------------ commands (from the UI side) */
static uint8_t live_note[EMB_NTRK][128];             /* voice + 1 of a held live key */
static uint32_t rec_start[EMB_NTRK][128];           /* the step a recorded note began, for its gate */

static void record_note(uint32_t trk, uint32_t note, uint32_t vel)
{
    deck_t *d = &deck[act];
    emb_track_t *t = &emb_bench.t[trk];
    emb_step_t *s;
    uint32_t k, step;
    int32_t micro;
    if (!d->on || d->song != &emb_bench)
        return;
    step = d->ts[trk];
    micro = (int32_t)d->tw[trk] - 12;               /* the tick in the step's window: its microtiming */
    if (micro >= 0 && d->tw[trk] > 12u + 9u) {       /* (late in the window: the next step, early) */
        step = step + 1u >= t->len ? 0u : step + 1u;
        micro -= EMB_TPS;
    }
    s = &t->s[step];
    for (k = 0; k < EMB_NNOTE; k++)
        if (!s->note[k] || s->note[k] == note)
            break;
    if (k == EMB_NNOTE)
        k = EMB_NNOTE - 1u;                         /* a full step: the last note is replaced */
    s->note[k] = (uint8_t)note;
    s->vel = (uint8_t)vel;
    s->micro = (int8_t)micro;
    s->gate = 4;
    s->lock = 255;
    rec_start[trk][note & 127u] = step | 0x100u;
}

static void record_off(uint32_t trk, uint32_t note)
{
    deck_t *d = &deck[act];
    uint32_t st = rec_start[trk][note & 127u], len = emb_bench.t[trk].len, q;
    if (!(st & 0x100u))
        return;
    rec_start[trk][note & 127u] = 0;
    st &= 0xFFu;
    q = d->ts[trk] >= st ? d->ts[trk] - st : d->ts[trk] + len - st;   /* steps held */
    q = q * 4u + (d->tw[trk] >> 3) + 1u;
    emb_bench.t[trk].s[st].gate = (uint8_t)(q > 254u ? 254u : q < 1u ? 1u : q);
}

static void command(const emb_cmd_t *c)
{
    switch (c->op) {
    case C_NOTE_ON: {
        const emb_track_t *t = &emb_bench.t[c->a & 3u];
        int note = c->b, root = (int)emb_bench.root;
        int vi;
        if (live_note[c->a & 3u][c->b & 127u])
            break;
        vi = play_note(2, c->a & 3u, t, note, c->c, t->tone, root, emb_bench.temper, 0);
        if (vi >= 0)
            live_note[c->a & 3u][c->b & 127u] = (uint8_t)(vi + 1);
        if (rec_on && playing)
            record_note(c->a & 3u, c->b, c->c);
        break;
    }
    case C_NOTE_OFF: {
        uint32_t tr = c->a & 3u, n = c->b & 127u, vi = live_note[tr][n];
        if (rec_on && playing)
            record_off(tr, n);
        if (!vi || ((hold_mask >> tr) & 1u))
            break;
        live_note[tr][n] = 0;
        if (vox[vi - 1u].on && vox[vi - 1u].note == n && vox[vi - 1u].deck == 2u)
            voice_release(&vox[vi - 1u]);
        break;
    }
    case C_PLAY:                                    /* a: the slot (255 workbench) */
        if (!playing || !deck[act].on) {
            playing = 1;
            tick_acc = 0;
            launch(c->a);
        } else if (deck[act].slot != c->a) {
            queue_slot = c->a;
        }
        break;
    case C_STOP:
        if (c->a) {                                 /* fade out over a bar */
            deck_stop(act, (int32_t)emb_udiv(16u * step_samples(), EMB_CTL));
            queue_slot = 0xFFFFu;
        } else {
            deck_stop(0, 0);
            deck_stop(1, 0);
            playing = 0;
            queue_slot = 0xFFFFu;
        }
        rec_on = 0;
        break;
    case C_QUEUE:
        if (!playing || !deck[act].on) {
            playing = 1;
            tick_acc = 0;
            launch(c->a);
        } else if (queue_slot == c->a) {
            queue_slot = 0xFFFFu;                   /* the same key again: never mind */
        } else {
            queue_slot = c->a;
        }
        break;
    case C_PURGE:                                   /* a: track, b: 1 = undo the last purge */
        if (c->b) {
            if (undo_valid) {
                emb_bench.t[undo_trk] = undo_track;
                undo_valid = 0;
            }
        } else {
            uint32_t tr = c->a & 3u, k;
            emb_track_t *t = &emb_bench.t[tr];
            undo_track = *t;                        /* in place: no allocation, nothing fragments */
            undo_trk = (uint8_t)tr;
            undo_valid = 1;
            for (k = 0; k < EMB_NSTEP; k++) {
                emb_step_t *s = &t->s[k];
                s->note[0] = s->note[1] = s->note[2] = s->note[3] = 0;
                s->micro = 0;
                s->lock = 255;
            }
            voices_release(-1, (int)tr, 1);         /* ~10 ms: no click */
        }
        break;
    case C_MOD:
        if (c->a == 1u || c->a == 9u) {             /* "home" and "full" also undo what is waiting */
            pend_mods_n = 0;
        }
        if (!playing)
            apply_mod(c->a);
        else if (pend_mods_n < sizeof pend_mods)
            pend_mods[pend_mods_n++] = c->a;
        break;
    case C_PANIC:
        voices_release(-1, -1, 1);
        deck_stop(0, 0);
        deck_stop(1, 0);
        playing = 0;
        queue_slot = 0xFFFFu;
        rec_on = 0;
        hold_mask = 0;
        {
            uint32_t t, n;
            for (t = 0; t < EMB_NTRK; t++)
                for (n = 0; n < 128u; n++)
                    live_note[t][n] = 0;
        }
        break;
    case C_HOLD:
        hold_mask ^= (uint8_t)(1u << (c->a & 3u));
        if (!((hold_mask >> (c->a & 3u)) & 1u)) {   /* let go of the latched drone */
            uint32_t n;
            for (n = 0; n < 128u; n++)
                if (live_note[c->a & 3u][n]) {
                    voice_t *v = &vox[live_note[c->a & 3u][n] - 1u];
                    if (v->on && v->deck == 2u && v->note == n)
                        voice_release(v);
                    live_note[c->a & 3u][n] = 0;
                }
        }
        break;
    case C_LESLIE:
        leslie_fast = (uint8_t)(c->b != 0u);
        break;
    case C_TEMPO:
        tempo_pm = clampi(c->v, 500, 2000);
        break;
    case C_FX:
        swell = (uint8_t)(c->a == 2u ? !swell : swell);
        rv_set(emb_set.rv_size, emb_set.rv_decay, emb_set.rv_damp, emb_set.rv_mix, swell);
        break;
    case C_INTENS:
        intensity = (uint8_t)(c->a > 3u ? 3u : c->a);
        break;
    case C_DUCK:
        duck = (uint8_t)(c->a != 0u);
        music_gain_t = c->v;
        break;
    case C_STINGER:
        if (playing)
            stinger_pending = 1;
        else
            stinger();
        break;
    case C_SEL_TRACK:
        rec_track = c->a & 3u;
        break;
    case C_RESYNC:                                  /* REC: arm / disarm live looping */
        rec_on = (uint8_t)(c->a != 0u);
        break;
    }
}

/* ------------------------------------------------------------------ the mixer */
static void track_gains(deck_t *d)
{
    static const uint8_t ON_AT[EMB_NTRK] = {1, 0, 2, 3};   /* intensity: the drone first, then drums, strings, lead */
    uint32_t k;
    for (k = 0; k < EMB_NTRK; k++) {
        int32_t t = 32767;
        if (d->slot != 255u) {
            if (intensity < ON_AT[k])
                t = 0;
            if (breakdown && k >= 2u)
                t = 0;
        } else if ((emb_mutes >> k) & 1u) {
            t = 0;
        }
        d->tg[k] += (t - d->tg[k]) >> 4;            /* ~25 ms */
    }
}

static int32_t softclip(int32_t x)                  /* gentle above -6 dBFS, hard at the rail */
{
    int32_t a = x < 0 ? -x : x, s = x < 0 ? -1 : 1;
    if (a > 16384) {
        a = 16384 + ((a - 16384) >> 1);
        if (a > 28000)
            a = 28000 + ((a - 28000) >> 2);
        if (a > 32767)
            a = 32767;
    }
    return a * s;
}

void emb_audio_init(void)
{
    uint32_t i;
    for (i = 0; i < EMB_NVOICE; i++)
        vox[i].slot = (uint8_t)i;
    vib_inc = 5u * 97391u * EMB_CTL;                /* 5 Hz per block */
    vib_depth = 0;
    act = 0;
    rv_set(emb_set.rv_size, emb_set.rv_decay, emb_set.rv_damp, emb_set.rv_mix, 0);
    for (i = 0; i < EMB_NTRK; i++) {
        les[i].inc = LES_SLOW;
        les[i].target = LES_SLOW;
    }
}

void emb_render(int32_t *lr, uint32_t frames)
{
    uint32_t f, i, k;
    for (f = 0; f + EMB_CTL <= frames; f += EMB_CTL) {
        int32_t peak = 0;
        uint32_t nvox = 0;
        while (cq_r != cq_w) {
            EMB_BARRIER();
            command(&cq[cq_r & (CQ - 1u)]);
            cq_r++;
        }
        clock_block();
        for (k = 0; k < 2u; k++) {                  /* the crossfade ramps */
            deck_t *d = &deck[k];
            if (!d->on)
                continue;
            if (d->gain < d->gain_t)
                d->gain = d->gain + d->gain_d > d->gain_t ? d->gain_t : d->gain + d->gain_d;
            else if (d->gain > d->gain_t)
                d->gain = d->gain - d->gain_d < d->gain_t ? d->gain_t : d->gain - d->gain_d;
            if (d->stopping && d->gain == 0) {
                d->on = 0;
                voices_release((int)k, -1, 1);
                if (k == act)
                    playing = 0;
            }
            track_gains(d);
        }
        {   /* the sympathetic strings follow the key of the front deck */
            const deck_t *a = &deck[act];
            uint32_t key = a->song ? (uint32_t)(deck_root(a) + 48) : 48u + emb_bench.root;
            if (key != cur_root_key) {
                cur_root_key = key;
                for (k = 0; k < EMB_NTRK; k++)
                    sym_tune(k, key);
            }
            vib_depth = a->song ? a->song->vib_depth : emb_bench.vib_depth;
            vib_inc = (2u + (uint32_t)(a->song ? a->song->vib_rate : emb_bench.vib_rate) / 16u) * 97391u * EMB_CTL;
        }
        for (k = 0; k < EMB_NTRK; k++)
            for (i = 0; i < EMB_CTL; i++)
                bus[k][i] = les_bus[k][i] = 0;
        for (i = 0; i < EMB_CTL; i++)
            mix_l[i] = mix_r[i] = send_l[i] = send_r[i] = 0;
        for (k = 0; k < EMB_NVOICE; k++) {
            voice_t *v = &vox[k];
            int32_t g;
            if (!v->on)
                continue;
            if (!voice_env_step(v)) {
                v->on = 0;
                continue;
            }
            nvox++;
            g = v->deck < 2u ? mul15(deck[v->deck].gain, deck[v->deck].tg[v->trk & 3u]) : 32767;
            voice_run(v, v->inst == I_TONEWHEEL ? les_bus[v->trk & 3u] : bus[v->trk & 3u], g);
        }
        for (k = 0; k < EMB_NTRK; k++) {            /* tracks: pan, sends, inserts */
            const emb_track_t *t = deck[act].on && deck[act].song ? &deck[act].song->t[k] : &emb_bench.t[k];
            int32_t lvl = (int32_t)t->level * 258, pl = emb_sine[(uint32_t)(127u - t->pan) * 2u + 1u],
                    pr = emb_sine[(uint32_t)t->pan * 2u + 1u], rs = (int32_t)t->rev * 258;
            /* (sine of 0..pi/2 over pan 0..127: equal power) */
            if (INST_ENG[t->inst] == ENG_PLUCK)
                sym_run(k, bus[k], 9000 + (int32_t)t->chr * 60);
            for (i = 0; i < EMB_CTL; i++) {
                int32_t s = mul15(bus[k][i], lvl), l = mul15(s, pl), r = mul15(s, pr);
                mix_l[i] += l;
                mix_r[i] += r;
                send_l[i] += mul15(l, rs);
                send_r[i] += mul15(r, rs);
            }
            les[k].target = leslie_fast ? LES_FAST : LES_SLOW;
            {
                int32_t any = 0;
                for (i = 0; i < EMB_CTL; i++)
                    any |= les_bus[k][i];
                if (any) {
                    int32_t tl[EMB_CTL], tr[EMB_CTL];
                    for (i = 0; i < EMB_CTL; i++)
                        tl[i] = tr[i] = 0;
                    for (i = 0; i < EMB_CTL; i++)
                        les_bus[k][i] = mul15(les_bus[k][i], lvl);
                    leslie_run(k, les_bus[k], tl, tr, pl, pr);
                    for (i = 0; i < EMB_CTL; i++) {
                        mix_l[i] += tl[i];
                        mix_r[i] += tr[i];
                        send_l[i] += mul15(tl[i], rs);
                        send_r[i] += mul15(tr[i], rs);
                    }
                }
            }
        }
        for (k = 0; k < 2u; k++)                    /* the beds: the hearth, the wind */
            if (deck[k].on && deck[k].song && deck[k].song->amb)
                amb_run(&amb_st[k], deck[k].song->amb, mix_l, mix_r, mul15(deck[k].gain, 12000));
        rv_run(send_l, send_r, mix_l, mix_r);
        music_gain += (music_gain_t - music_gain) >> 5;
        {
            int32_t g = (int32_t)emb_master_vol * 128;
            if (duck)
                g = mul15(g, 9000);
            g = mul15(g, music_gain);
            for (i = 0; i < EMB_CTL; i++) {
                int32_t l = softclip(mul15(mix_l[i], g)), r = softclip(mul15(mix_r[i], g));
                lr[2u * (f + i)] = l;
                lr[2u * (f + i) + 1u] = r;
                if (l > peak)
                    peak = l;
            }
        }
        emb_st.peak = (int16_t)peak;
        emb_st.voices = (uint8_t)nvox;
    }
    /* the status the UI reads */
    for (k = 0; k < 2u; k++) {
        uint32_t t;
        for (t = 0; t < EMB_NTRK; t++)
            emb_st.step[k][t] = deck[k].ts[t];
        emb_st.deck_slot[k] = deck[k].on ? deck[k].slot : 254u;
    }
    emb_st.playing = playing;
    emb_st.active = (uint8_t)act;
    emb_st.queued = queue_slot == 0xFFFFu ? 0u : queue_slot == 255u ? 255u : (uint8_t)(queue_slot + 1u);
    emb_st.fading = deck[act ^ 1u].on;
    emb_st.beat = (uint8_t)((deck[act].gstep >> 2) & 3u);
    emb_st.intensity = intensity;
    emb_st.duck = duck;
    emb_st.mod_mode = (uint8_t)(mode_over < 0 ? 255 : mode_over);
    emb_st.key_shift = (uint8_t)key_shift;
    emb_st.rate_t1 = rate_t1;
    emb_st.hold = hold_mask | (uint8_t)(rec_on << 7) | (uint8_t)(breakdown << 6);
    emb_st.leslie = leslie_fast;
    emb_st.swell = swell;
}
