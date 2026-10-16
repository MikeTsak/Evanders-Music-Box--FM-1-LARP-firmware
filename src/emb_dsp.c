/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: the sound. Fixed-point voices (strings, bowed, choir, winds, organs, drums),
 * the sympathetic strings, the SANCTUARY reverb and the Leslie rotor. Audio side only (see emb.h).
 * Part of the unity build (src/emb.c). */

/* ------------------------------------------------------------------ helpers */
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }

uint32_t emb_udiv(uint32_t a, uint32_t b)          /* shift-subtract: no divide instruction or libcall */
{
    uint32_t q = 0, r = 0;
    int i;
    if (!b)
        return 0xFFFFFFFFu;
    for (i = 31; i >= 0; i--) {
        r = r << 1 | ((a >> i) & 1u);
        if (r >= b) {
            r -= b;
            q |= 1u << i;
        }
    }
    return q;
}

static uint32_t pow48_div(uint32_t b)              /* 2^48 / b, b < 2^31: 32-bit steps only (no 64-bit libcalls) */
{
    uint32_t r = 0, q = 0;
    int i;
    if (!b)
        return 0xFFFFFFFFu;
    for (i = 48; i >= 0; i--) {
        r = r << 1 | (uint32_t)(i == 48);           /* r < b < 2^31: never overflows */
        if (r >= b) {
            r -= b;
            if (i < 32)
                q |= 1u << i;
        }
    }
    return q;
}

uint32_t emb_sum(const void *p, uint32_t n)        /* Fletcher-style: catches torn and blank records */
{
    const uint8_t *b = p;
    uint32_t a = 1, s = 0;
    while (n--) {
        a = (a + *b++) & 0xFFFFu;
        s = (s + a) & 0xFFFFu;
    }
    return s << 16 | a;
}

int16_t emb_sine[1024];

static void sine_init(void)                         /* resonator recurrence, Q30: no float, no table in flash */
{
    int64_t c2 = 2 * 1073721611LL, s0 = 0, s1 = 6588356, s2;   /* 2cos(2pi/1024), sin(2pi/1024) */
    uint32_t i;
    emb_sine[0] = 0;
    for (i = 1; i < 1024u; i++) {
        emb_sine[i] = (int16_t)clampi((int32_t)((s1 + (1 << 14)) >> 15), -32767, 32767);
        s2 = ((c2 * s1) >> 30) - s0;
        s0 = s1;
        s1 = s2;
    }
}
static inline int32_t sin_ph(uint32_t ph) { return emb_sine[ph >> 22]; }

static uint32_t nz_state = 0x9E3779B9u;
static inline int32_t noise(uint32_t *s)            /* xorshift32 -> Q15 */
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (int32_t)x >> 16;
}

/* 2^(i/64), Q30 */
static const uint32_t EXP2_64[65] = {
    1073741824, 1085434106, 1097253708, 1109202018, 1121280436, 1133490379, 1145833280, 1158310587, 1170923762,
    1183674286, 1196563654, 1209593378, 1222764986, 1236080024, 1249540052, 1263146652, 1276901417, 1290805962,
    1304861917, 1319070932, 1333434672, 1347954824, 1362633090, 1377471191, 1392470869, 1407633882, 1422962010,
    1438457051, 1454120821, 1469955159, 1485961921, 1502142985, 1518500250, 1535035634, 1551751076, 1568648537,
    1585730000, 1602997467, 1620452965, 1638098541, 1655936265, 1673968228, 1692196547, 1710623359, 1729250827,
    1748081133, 1767116489, 1786359126, 1805811301, 1825475297, 1845353420, 1865448001, 1885761398, 1906295993,
    1927054196, 1948038440, 1969251188, 1990694927, 2012372174, 2034285470, 2056437387, 2078830522, 2101467502,
    2124350982, 2147483648u};
static uint32_t exp2_frac(uint32_t f16)             /* 2^(f16 / 65536), f16 < 65536 -> Q30 in [1, 2) */
{
    uint32_t i = f16 >> 10, r = f16 & 1023u, a = EXP2_64[i], b = EXP2_64[i + 1];
    return a + (uint32_t)(((uint64_t)(b - a) * r) >> 10);
}
static uint32_t exp2_neg_q16(uint32_t x16)          /* 2^(-x), x Q16 -> Q16 */
{
    uint32_t n = x16 >> 16, f = x16 & 0xFFFFu, m;
    if (n >= 16u)
        return 0;
    m = f ? exp2_frac(65536u - f) : 0x80000000u;   /* 2^(1 - f), Q30 */
    return (m >> 14) >> (n + 1u);
}

/* phase increment of a pitch: note Q8 (MIDI note * 256), + cents. INC0 = MIDI note 0 (8.1758 Hz) */
#define INC0 796254u
static uint32_t pitch_inc(int32_t note_q8, int32_t cents)
{
    int32_t x;
    uint32_t n, m;
    note_q8 += (cents * 256) / 100;                 /* (by a constant) */
    note_q8 = clampi(note_q8, 0, 135 * 256);
    x = (note_q8 * 64) / 3;                         /* note / 12 in octaves, Q16 (by a constant) */
    n = (uint32_t)x >> 16;
    m = exp2_frac((uint32_t)x & 0xFFFFu);
    return (uint32_t)(((uint64_t)INC0 * m) >> 30) << n;
}

/* one-pole decay per control block to reach -60 dB in t_ms: Q16 multiplier */
static uint32_t decay_k(uint32_t t_ms)
{
    if (t_ms < 1u)
        t_ms = 1u;
    return exp2_neg_q16(emb_udiv(473940u, t_ms));   /* 2^(-9.966 * CTL * 1000 / (FS * t)) */
}

/* envelope/decay times for 0..127: 2 ms * 2^(p / 10), up to ~13 s */
static uint32_t time_ms(uint32_t p)
{
    uint32_t x = p * 6554u;                         /* p / 10 in Q16 */
    return exp2_frac(x & 0xFFFFu) >> (29u - (x >> 16));   /* 2 x 2^(f) Q30 = Q29; x >> 16 <= 12 */
}

/* inc * 2^x, x in octaves Q16 (|x| < 8 octaves) */
static uint32_t inc_oct(uint32_t inc, int32_t x)
{
    uint32_t r = (uint32_t)(((uint64_t)inc * exp2_frac((uint32_t)x & 0xFFFFu)) >> 30);   /* (fits: inc < 2^31) */
    int32_t n = x >> 16;                            /* floor */
    return n >= 0 ? r << n : r >> -n;
}
static inline int32_t cents_oct(int32_t c) { return c * 54 + ((c * 157) >> 8); }   /* cents -> octaves Q16 */

/* SVF coefficient from Hz (Q15), clamped where the Chamberlin form stays stable */
static inline int32_t svf_f(int32_t hz) { return clampi((hz * 4781) >> 10, 10, 26000); }
typedef struct { int32_t lp, bp; } svf_t;
static inline int32_t svf_bp(svf_t *s, int32_t in, int32_t f, int32_t q)
{
    int32_t hp;
    s->lp += (int32_t)(((int64_t)f * s->bp) >> 15);
    hp = in - s->lp - (int32_t)(((int64_t)q * s->bp) >> 15);
    s->bp += (int32_t)(((int64_t)f * hp) >> 15);
    s->bp = clampi(s->bp, -2000000, 2000000);       /* a bound, never reached in tune */
    return s->bp;
}

/* polyBLEP saw, Q15. r: 2^31 / dt (dt = inc >> 16), set at note-on */
static inline int32_t saw_blep(uint32_t ph, uint32_t inc, uint32_t r)
{
    int32_t t = (int32_t)(ph >> 16), dt = (int32_t)(inc >> 16), s = t * 2 - 65536, x;
    if (t < dt) {
        x = (int32_t)(((uint32_t)t * r) >> 16);     /* t / dt, Q15 */
        s -= (2 * x - ((x * x) >> 15) - 32768) * 2;
    } else if (t > 65536 - dt) {
        x = -(int32_t)(((uint32_t)(65536 - t) * r) >> 16);
        s -= (((x * x) >> 15) + 2 * x + 32768) * 2;
    }
    return s >> 1;
}

/* ------------------------------------------------------------------ instruments */
enum { ENG_PLUCK, ENG_BOW, ENG_CHOIR, ENG_WIND, ENG_ORGAN, ENG_DRUM };
static const uint8_t INST_ENG[I_COUNT] = {
    ENG_DRUM, ENG_DRUM, ENG_DRUM,
    ENG_PLUCK, ENG_PLUCK, ENG_PLUCK, ENG_PLUCK, ENG_PLUCK,
    ENG_CHOIR, ENG_BOW, ENG_BOW, ENG_BOW,
    ENG_WIND, ENG_WIND, ENG_WIND, ENG_WIND,
    ENG_ORGAN, ENG_ORGAN, ENG_ORGAN, ENG_ORGAN};

const char *const emb_inst_name[I_COUNT] = {
    "TAVERN KIT", "WAR KIT", "RITUAL KIT",
    "PHYS HARP", "LUTE", "NYLON GTR", "KOTO", "PSALTERY",
    "CHOIR", "HURDY GURDY", "BAGPIPE", "LOW CELLO",
    "PAN FLUTE", "RECORDER", "SHAWM", "WAR BRASS",
    "ORGANETTO", "POSITIVE", "CATHEDRAL", "TONEWHEEL"};
const char *const emb_drum_name[DR_COUNT] = {"BODHRAN", "FRAME DRUM", "TAMBOURINE", "ANVIL",
                                             "TIMPANI", "WAR DRUM", "CLACK", "GONG"};

/* organ ranks: harmonic multiples in eighths (16' = 4, 8' = 8, 5 1/3' = 12, 4' = 16, 2 2/3' = 24, 2' = 32,
 * 1 3/5' = 40, 1 1/3' = 48, 1' = 64) and the stops each organ draws (Q15) */
static const uint8_t RANK_X8[9] = {4, 12, 8, 16, 24, 32, 40, 48, 64};
static const uint16_t ORGAN_STOPS[3][9] = {
    {0, 0, 22000, 9000, 0, 2500, 0, 0, 0},                    /* organetto: 8' 4' (2') */
    {0, 0, 16000, 11000, 0, 8000, 0, 4000, 2500},             /* positive: 8' 4' 2' 1 1/3' 1' */
    {12000, 3000, 16000, 10000, 6000, 6000, 0, 3500, 3000},   /* cathedral: 16' .. mixture */
};

/* drum pieces: base Hz, partial ratios (x/256), amps, decays (ms); noise amp, Hz, q, decay; drop (semitones Q8, ms) */
typedef struct {
    uint16_t hz, ratio[4], amp[4], dec[4];
    uint16_t n_amp, n_hz, n_q, n_dec;
    uint16_t drop, drop_ms;
} drum_t;
static const drum_t DRUM[DR_COUNT] = {
    /* bodhran: deep goatskin, a pitch drop, a thump */
    {78, {256, 407, 546, 0}, {22000, 6000, 2500, 0}, {260, 150, 90, 1}, 9000, 400, 20000, 50, 7 * 256, 40},
    /* frame drum: circular membrane modes */
    {150, {256, 407, 548, 640}, {15000, 8000, 5000, 3000}, {380, 220, 160, 120}, 7000, 1800, 14000, 30, 3 * 256, 25},
    /* tambourine: jingles (inharmonic, fast) over bright noise */
    {2300, {256, 367, 541, 718}, {3000, 2600, 2400, 2000}, {140, 120, 110, 90}, 12000, 7200, 9000, 150, 0, 1},
    /* anvil: a bar's modes, long ring */
    {820, {256, 707, 1382, 2286}, {14000, 9000, 6000, 3000}, {1500, 900, 600, 400}, 9000, 3500, 6000, 8, 0, 1},
    /* timpani: tuned (the song's root), slightly inharmonic overtones */
    {98, {256, 384, 507, 625}, {20000, 10000, 6000, 3500}, {2200, 1500, 1000, 700}, 5000, 300, 16000, 40, 256, 60},
    /* war drum: huge, low, pitch dives */
    {55, {256, 397, 563, 0}, {26000, 7000, 3000, 0}, {620, 300, 160, 1}, 12000, 250, 22000, 80, 12 * 256, 70},
    /* clack: wood on wood */
    {1750, {256, 0, 0, 0}, {8000, 0, 0, 0}, {35, 1, 1, 1}, 14000, 1800, 5000, 30, 0, 1},
    /* gong: slow bloom, very long */
    {110, {256, 353, 538, 727}, {12000, 9000, 7000, 5000}, {6000, 4500, 3500, 2500}, 3000, 900, 12000, 400, 0, 1},
};
/* loudness, Q13 (8192 = as rendered): measured so one voice sits near -14 dBFS RMS (drums: peaks near 24000),
 * so a track's LEVEL means the same on every instrument (averaged over four notes) */
static const uint16_t INST_GAIN[I_COUNT] = {
    8192, 8192, 8192,
    14115, 24122, 16788, 15458, 12988,             /* strings: a pluck's energy is all at the start */
    6753, 4969, 3214, 3933,                         /* choir, gurdy, bagpipe, cello */
    2624, 2609, 5057, 3328,                         /* pan flute, recorder, shawm, brass */
    6339, 7041, 6361, 3732};                        /* organs */
static const uint16_t DRUM_GAIN[DR_COUNT] = {7800, 7000, 5700, 5700, 6100, 7000, 20480, 4900};

/* per kit: base pitch (x/256) and decay (x/256) */
static const uint16_t KIT_TUNE[3][2] = {{280, 200}, {220, 330}, {200, 420}};

/* ------------------------------------------------------------------ voices */
#define KS_MAX 1024u                                /* string delay: down to ~43 Hz (F1) */
typedef struct {
    uint8_t on, eng, inst, trk, deck, note, stage, piece;
    uint8_t tone, chr, slot;                        /* slot: the KS / tube buffer */
    int32_t vel;                                    /* Q15, includes the step velocity */
    uint32_t age;
    int32_t gate;                                   /* samples until the release, < 0 none */
    int32_t env, env0;                              /* Q24 at this block's end / start */
    int32_t atk, sus;                               /* Q24 per block; Q24 */
    uint32_t dk, rk;                                /* Q16 per block */
    uint32_t ph[9], inc[9];
    int32_t pa[9];                                  /* partial amplitude, Q24 (drums) or rank gain Q15 */
    uint32_t pk[4];                                 /* drum partial decay per block, Q16 */
    uint32_t r;                                     /* polyBLEP reciprocal */
    svf_t f1, f2, f3;
    int32_t f1f, f1q, f2f, f2q, f3f, f3q, lp;
    uint32_t wob_ph, wob_inc;                       /* a slow wander: an organ's bellows, a player's breath */
    int32_t wob_amt;                                /* its depth, cents */
    uint32_t base_inc;                              /* without vibrato / pitch env */
    int32_t pdrop, pdrop_k;                         /* pitch drop Q16 of an octave, decay per block Q16 */
    int32_t nz_a;                                   /* noise / chiff level, Q24 */
    uint32_t nz_k, nz;
    /* strings */
    uint32_t d_q16;                                 /* delay length Q16 */
    uint16_t w;
    int32_t ks_g, ks_x1, ks_x2;
} voice_t;

static voice_t vox[EMB_NVOICE];
static int16_t ks_buf[EMB_NVOICE][KS_MAX] EMB_POOL;
static uint32_t vox_age;
static uint32_t coup_ph;                            /* the hurdy-gurdy "coup": eighth notes, set by the clock */
static uint32_t vib_inc, vib_ph;                    /* shared vibrato LFO */
static int32_t vib_depth;                           /* cents */

static void voice_env(voice_t *v, uint32_t a, uint32_t d, uint32_t s, uint32_t r)   /* 0..127 each */
{
    uint32_t t = time_ms(a);
    v->atk = (int32_t)emb_udiv(12174284u, t);       /* (1 << 24) * CTL * 1000 / (FS * t) */
    if (v->atk > (1 << 24))
        v->atk = 1 << 24;
    v->dk = decay_k(time_ms(d));
    v->sus = (int32_t)(s * 132104u);                /* /127 -> Q24 */
    v->rk = decay_k(time_ms(r));
    v->stage = 0;
    v->env = v->env0 = 0;
}

/* the delay of a string at inc: period in samples Q16 = 2^48 / inc, minus the loop filter's delay */
static uint32_t ks_delay(uint32_t inc, uint32_t filt_q16)
{
    uint32_t d = pow48_div(inc);
    d = d > filt_q16 + 0x20000u ? d - filt_q16 : 0x20000u;
    return d > (KS_MAX - 4u) << 16 ? (KS_MAX - 4u) << 16 : d;
}

static int bright_string(uint32_t inst) { return inst == I_HARP || inst == I_KOTO || inst == I_PSALTERY; }

/* start voice v: inst on a MIDI note (drums: piece in v->piece, note = the tuning note) */
static void voice_start(voice_t *v, const emb_track_t *t, uint32_t inst, uint32_t note, int32_t cents,
                        int32_t vel_q15, uint32_t tone)
{
    uint32_t i, eng = INST_ENG[inst];
    uint8_t slot = v->slot, piece = v->piece;       /* (the buffer and the drum piece survive the reset) */
    uint32_t nz = v->nz ? v->nz : 0x1234567u + (uint32_t)(v - vox) * 7919u;
    for (i = 0; i < sizeof *v; i++)
        ((uint8_t *)v)[i] = 0;
    v->slot = slot;
    v->piece = piece;
    v->nz = nz;
    v->on = 1;
    v->eng = (uint8_t)eng;
    v->inst = (uint8_t)inst;
    v->note = (uint8_t)note;
    v->tone = (uint8_t)tone;
    v->chr = t->chr;
    v->vel = vel_q15;
    v->age = ++vox_age;
    v->gate = -1;
    v->base_inc = pitch_inc((int32_t)note << 8, cents);
    voice_env(v, t->att, t->dec, t->sus, t->rel);
    switch (eng) {
    case ENG_PLUCK: {
        int16_t *b = ks_buf[v->slot];
        uint32_t bright = bright_string(inst), len, pos, k;
        int32_t a = 6000 + (int32_t)tone * 200, y = 0, x;   /* excitation lowpass: brightness */
        v->d_q16 = ks_delay(v->base_inc, bright ? 0x8000u : 0x10000u);
        len = (v->d_q16 >> 16) + 2u;
        pos = (len * (16u + v->chr)) >> 8;          /* CHARACTER: pluck position (comb) */
        for (k = 0; k < KS_MAX; k++)
            b[k] = 0;
        for (k = 0; k < len; k++) {
            x = noise(&v->nz);
            y += mul15(x - y, a);
            b[k] = (int16_t)clampi(y, -32767, 32767);
        }
        for (k = len - 1u; k >= pos && pos; k--)
            b[k] = (int16_t)((b[k] - b[k - pos]) >> 1);
        v->w = (uint16_t)len;
        /* loop gain: per period, so the decay time stays similar over the range */
        v->ks_g = inst == I_LUTE ? 32590 : inst == I_NYLON ? 32660 : inst == I_KOTO ? 32700 :
                  inst == I_PSALTERY ? 32740 : 32735;
        v->atk = 1 << 24;                           /* a pluck has no attack: the string is the envelope */
        v->sus = 1 << 24;
        if (inst == I_KOTO) {
            v->pdrop = cents_oct(40);               /* the twang: starts 40 cents sharp, settles */
            v->pdrop_k = (int32_t)decay_k(120);
        }
        {   /* the body: two resonances of the instrument's box or soundboard (Hz), and the quill's click */
            static const uint16_t BODY[5][2] = {{190, 480}, {105, 290}, {100, 235}, {165, 700}, {330, 1250}};
            const uint16_t *bd = BODY[inst - I_HARP];
            v->f1f = svf_f(bd[0]);
            v->f2f = svf_f(bd[1]);
            v->f1q = 5000;
            v->f2q = 7000;
            v->nz_a = (int32_t)(1800 + tone * 40) << 9;   /* the plectrum (a quill, a nail): a few ms of bright noise */
            v->nz_k = decay_k(inst == I_HARP ? 3 : 6);
        }
        break;
    }
    case ENG_BOW:
    case ENG_CHOIR:
        v->r = emb_udiv(0x80000000u, (v->base_inc >> 16) | 1u);
        v->ph[1] = v->nz;                           /* the second (detuned) oscillator, free phase */
        v->ph[2] = v->nz * 7u;                      /* a third: the choir's third singer, the gurdy's chanterelle */
        if (inst == I_CHOIR) {                      /* vowels: TONE moves ah -> oh -> oo */
            int32_t m = (int32_t)tone;
            v->f1f = svf_f(730 - m * 3);
            v->f2f = svf_f(1100 - m * 2);
            v->f3f = svf_f(2550 - m * 2);
            v->f1q = 9000;
            v->f2q = 9000;
            v->f3q = 12000;
        } else if (inst == I_BAGPIPE) {
            v->f1f = svf_f(950);
            v->f2f = svf_f(2600);
            v->f1q = 7000;
            v->f2q = 9000;
            v->pdrop = cents_oct(200);              /* the skirl: a grace note a tone above, gone in ~30 ms */
            v->pdrop_k = (int32_t)decay_k(30);
        } else if (inst == I_CELLO) {
            v->f1f = svf_f(280);
            v->f2f = svf_f(1100 + (int32_t)tone * 6);
            v->f1q = 12000;
            v->f2q = 14000;
        } else {                                    /* hurdy-gurdy: the nasal wheel string */
            v->f1f = svf_f(520);
            v->f2f = svf_f(1700 + (int32_t)tone * 8);
            v->f3f = svf_f(2300);                   /* the rosin on the wheel */
            v->f1q = 9000;
            v->f2q = 10000;
            v->f3q = 9000;
        }
        break;
    case ENG_WIND:
        v->r = emb_udiv(0x80000000u, (v->base_inc >> 16) | 1u);
        v->nz_a = 1 << 24;                          /* the chiff */
        v->nz_k = decay_k(inst == I_PANFLUTE ? 140 : 70);
        {
            uint32_t hz2 = (uint32_t)(((uint64_t)v->base_inc * EMB_FS) >> 31);   /* 2 x the fundamental, Hz */
            v->f1f = svf_f((int32_t)hz2);
        }
        v->f1q = 4000;
        v->f2f = svf_f(inst == I_SHAWM ? 1300 : 600 + (int32_t)tone * 40);
        v->f2q = inst == I_SHAWM ? 6000 : 20000;
        v->f3f = svf_f(2800);                       /* the shawm's second, nasal formant */
        v->f3q = 8000;
        if (inst == I_BRASS) {
            v->pdrop = cents_oct(-25);              /* the blat: starts flat */
            v->pdrop_k = (int32_t)decay_k(60);
        } else {
            v->pdrop = cents_oct(inst == I_SHAWM ? -30 : -18);   /* the breath scoops up into the note */
            v->pdrop_k = (int32_t)decay_k(inst == I_SHAWM ? 40 : 55);
            v->wob_inc = (5u + (v->nz & 3u)) * 97391u * EMB_CTL;   /* the player's breath: 5..8 Hz */
            v->wob_amt = inst == I_SHAWM ? 4 : 3;
        }
        break;
    case ENG_ORGAN: {
        uint32_t o = inst - I_ORGANETTO;
        for (i = 0; i < 9u; i++) {
            uint64_t inc = ((uint64_t)v->base_inc * RANK_X8[i]) >> 3;
            int32_t g = o < 3u ? ORGAN_STOPS[o][i] : 0;
            if (o == 3u)                            /* tonewheel: the four drawbar groups */
                g = (int32_t)t->draw[i < 2u ? 0 : i < 4u ? 1 : i < 6u ? 2 : 3] * 2600;
            v->inc[i] = inc < 0x7FFFFFFFull ? (uint32_t)inc : 0;
            v->pa[i] = v->inc[i] ? g : 0;
            v->ph[i] = o == 3u ? (uint32_t)i * 0x1C71C71Cu : 0;   /* tonewheels never start in step */
        }
        if (o == 2u)                                /* a cathedral's mixture: the upper ranks beat gently */
            for (i = 4u; i < 9u; i++)
                v->inc[i] += (i & 1u) ? v->inc[i] >> 11 : (uint32_t)-(int32_t)(v->inc[i] >> 11);
        if (o < 2u) {                               /* a small organ's bellows: the wind is never quite steady */
            v->wob_inc = (o ? 20000u : 26000u) * EMB_CTL + (v->nz & 0x3FFFu) * EMB_CTL;
            v->wob_ph = v->nz << 7;
            v->wob_amt = o ? 2 : 5;
        }
        v->nz_a = o == 3u ? 1 << 22 : 1 << 24;      /* key click / pipe chiff */
        v->nz_k = decay_k(o == 3u ? 12 : 90);
        v->f1f = svf_f(clampi((int32_t)(((uint64_t)v->base_inc * EMB_FS * 3u) >> 32), 200, 5000));
        v->f1q = 5000;
        if (o == 2u && t->att < 40u)                /* a cathedral's big pipes take time to speak */
            v->atk = (int32_t)emb_udiv(12174284u, 120u);
        break;
    }
    case ENG_DRUM: {
        const drum_t *d = &DRUM[v->piece];
        uint32_t kit = inst <= I_RITUAL_KIT ? inst : 0, tune = KIT_TUNE[kit][0], dec = KIT_TUNE[kit][1];
        uint32_t base = v->piece == DR_TIMPANI || v->piece == DR_GONG ? v->base_inc :
                        (uint32_t)(((uint64_t)d->hz * 97391u * tune) >> 8);   /* Hz -> inc: 2^32 / FS = 97391 */
        if (v->piece == DR_TIMPANI || v->piece == DR_WAR)
            dec = dec + 100u;
        v->base_inc = base;
        for (i = 0; i < 4u; i++) {
            v->inc[i] = (uint32_t)(((uint64_t)base * d->ratio[i]) >> 8);
            v->pa[i] = (int32_t)d->amp[i] << 9;     /* Q24 */
            v->pk[i] = decay_k((d->dec[i] * dec) >> 8);
        }
        v->nz_a = (int32_t)d->n_amp << 9;
        v->nz_k = decay_k((d->n_dec * dec) >> 8);
        v->f1f = svf_f(d->n_hz);
        v->f1q = d->n_q;
        v->pdrop = (int32_t)((d->drop * 65536u) / (12u * 256u));
        v->pdrop_k = (int32_t)decay_k(d->drop_ms);
        v->atk = 1 << 24;
        v->sus = 0;
        v->dk = decay_k(((uint32_t)d->dec[0] * dec) >> 7);   /* the whole voice: twice the main mode */
        break;
    }
    }
    v->vel = (int32_t)(((int64_t)vel_q15 * (eng == ENG_DRUM ? DRUM_GAIN[v->piece & 7u] : INST_GAIN[inst])) >> 13);
}

static void voice_release(voice_t *v)
{
    if (v->on && v->stage < 3u)
        v->stage = 3;
}

/* advance the envelope one block; 0 when the voice has ended */
static int voice_env_step(voice_t *v)
{
    v->env0 = v->env;
    if (v->gate >= 0) {
        v->gate -= (int32_t)EMB_CTL;
        if (v->gate < 0)
            voice_release(v);
    }
    switch (v->stage) {
    case 0:
        v->env += v->atk;
        if (v->env >= 1 << 24) {
            v->env = 1 << 24;
            v->stage = 1;
        }
        break;
    case 1:
    case 2:
        v->env = v->sus + (int32_t)(((int64_t)(v->env - v->sus) * v->dk) >> 16);
        break;
    default:
        v->env = (int32_t)(((int64_t)v->env * v->rk) >> 16);
        break;
    }
    if (v->eng == ENG_PLUCK && v->stage < 3u)       /* strings ring on: the loop decays them */
        v->env = 1 << 24;
    if (v->eng == ENG_PLUCK && v->stage == 3u && v->rk > 64000u)
        v->rk = 64000u;                             /* a released string is damped, never longer */
    return v->env > 400 || v->stage == 0;
}

/* pitch per block: vibrato and pitch drops (pdrop: octaves Q16, decaying) */
static uint32_t voice_inc(voice_t *v, int32_t vib_q15)
{
    int32_t x = 0;
    if (v->pdrop) {
        x = v->pdrop;
        v->pdrop = (int32_t)(((int64_t)v->pdrop * v->pdrop_k) >> 16);
    }
    if (v->eng == ENG_BOW || v->eng == ENG_CHOIR || v->eng == ENG_WIND)
        x += cents_oct((vib_q15 * vib_depth) >> 15);
    if (v->wob_amt) {
        v->wob_ph += v->wob_inc;
        x += cents_oct((sin_ph(v->wob_ph) * v->wob_amt) >> 15);
    }
    return x ? inc_oct(v->base_inc, x) : v->base_inc;
}

/* render one block of v into out (mono, Q15, added) */
static void voice_run(voice_t *v, int32_t *out, int32_t gain_q15)
{
    uint32_t i, inc = 0;
    int32_t e0 = v->env0 >> 9, de = ((v->env >> 9) - e0) >> 5;   /* Q15, the ramp over the block */
    int32_t g = mul15(v->vel, gain_q15);
    if (v->eng == ENG_BOW || v->eng == ENG_CHOIR || v->eng == ENG_WIND)
        inc = voice_inc(v, sin_ph(vib_ph));
    switch (v->eng) {
    case ENG_PLUCK: {
        int16_t *b = ks_buf[v->slot];
        uint32_t w = v->w, bright = bright_string(v->inst);
        if (v->pdrop)
            v->d_q16 = ks_delay(voice_inc(v, 0), bright ? 0x8000u : 0x10000u);
        for (i = 0; i < EMB_CTL; i++) {
            uint32_t rp = (w << 16) - v->d_q16, i0 = (rp >> 16) & (KS_MAX - 1u), fr = rp & 0xFFFFu;
            int32_t a = b[i0], c = b[(i0 + 1u) & (KS_MAX - 1u)], x = a + (int32_t)(((int64_t)(c - a) * fr) >> 16), y;
            if (bright)
                y = (x + v->ks_x1) >> 1;
            else
                y = (x + 2 * v->ks_x1 + v->ks_x2) >> 2;
            v->ks_x2 = v->ks_x1;
            v->ks_x1 = x;
            y = mul15(y, v->ks_g);
            b[w & (KS_MAX - 1u)] = (int16_t)clampi(y, -32767, 32767);
            w++;
            {
                int32_t n = noise(&v->nz), body, click;
                body = svf_bp(&v->f1, x >> 3, v->f1f, v->f1q) + svf_bp(&v->f2, x >> 3, v->f2f, v->f2q);
                v->lp += (n - v->lp) >> 2;          /* the click: noise above ~1.5 kHz */
                click = (int32_t)(((int64_t)(n - v->lp) * (v->nz_a >> 9)) >> 15);
                out[i] += mul15(mul15(x + (body >> 1) + click, e0 + de * (int32_t)i), g);
            }
        }
        v->nz_a = (int32_t)(((int64_t)v->nz_a * v->nz_k) >> 16);
        v->w = (uint16_t)(w & (KS_MAX - 1u));
        break;
    }
    case ENG_BOW:
    case ENG_CHOIR: {
        uint32_t inc2 = inc + (inc >> 9), inc3 = inc - (inc >> 10);   /* +3.4 and -1.7 cents: more strings, singers */
        int32_t buzz = 0, wet, three = v->inst == I_CHOIR || v->inst == I_GURDY;
        uint32_t cp = coup_ph;
        for (i = 0; i < EMB_CTL; i++) {
            int32_t s, f;
            if (v->inst == I_BAGPIPE)
                s = saw_blep(v->ph[0], inc, v->r) - saw_blep(v->ph[0] + 0x50000000u, inc, v->r);   /* narrow reed */
            else if (three)
                s = ((saw_blep(v->ph[0], inc, v->r) + saw_blep(v->ph[1], inc2, v->r) +
                      saw_blep(v->ph[2], inc3, v->r)) * 10923) >> 15;
            else
                s = (saw_blep(v->ph[0], inc, v->r) + saw_blep(v->ph[1], inc2, v->r)) >> 1;
            v->ph[0] += inc;
            v->ph[1] += inc2;
            v->ph[2] += inc3;
            f = svf_bp(&v->f1, s >> 1, v->f1f, v->f1q) + svf_bp(&v->f2, s >> 1, v->f2f, v->f2q);
            if (v->inst == I_CHOIR)                 /* the third formant and the breath */
                f += svf_bp(&v->f3, (s >> 2) + (noise(&v->nz) >> 4), v->f3f, v->f3q);
            else if (v->inst == I_GURDY)            /* rosin: a scratch that swells with each turn of the wheel */
                f += mul15(svf_bp(&v->f3, noise(&v->nz) >> 3, v->f3f, v->f3q),
                           3000 + (int32_t)((cp >> 16) < 9000u ? (9000u - (cp >> 16)) : 0u));
            v->lp += (s - v->lp) >> 3;
            wet = f + (v->lp >> 1);
            if (v->inst == I_GURDY && v->chr > 8u) {   /* the trompette: a buzzing bridge, struck on each coup */
                uint32_t cph = cp >> 16;
                int32_t ce = cph < 9000u ? (int32_t)(9000u - cph) * 3 : 0, z = s > 6000 ? 12000 : s < -6000 ? -12000 : s * 2;
                buzz = mul15(z, mul15(ce, (int32_t)v->chr * 258));
                cp += 0x10000u;
                wet += buzz;
            }
            out[i] += mul15(mul15(wet, e0 + de * (int32_t)i), g);
        }
        break;
    }
    case ENG_WIND: {
        int32_t breath = 2000 + (int32_t)v->chr * 90;
        for (i = 0; i < EMB_CTL; i++) {
            int32_t s, n = noise(&v->nz), ch = (int32_t)(((int64_t)n * (v->nz_a >> 9)) >> 15);
            if (v->inst == I_PANFLUTE || v->inst == I_RECORDER) {
                s = sin_ph(v->ph[0]);
                s += sin_ph(v->ph[0] * 2u) / (v->inst == I_RECORDER ? 5 : 8);
                s += sin_ph(v->ph[0] * 3u) / 20;
                s += mul15(svf_bp(&v->f1, n, v->f1f, v->f1q), v->inst == I_PANFLUTE ? breath * 2 : breath);
                s += svf_bp(&v->f2, ch, v->f1f, 6000) >> 1;   /* chiff at the attack */
            } else if (v->inst == I_SHAWM) {         /* a double reed: buzzing, two nasal formants, hard-driven */
                s = saw_blep(v->ph[0], inc, v->r) - saw_blep(v->ph[0] + 0x40000000u, inc, v->r);
                s = (s >> 1) + svf_bp(&v->f2, s >> 1, v->f2f, v->f2q) + (svf_bp(&v->f3, s >> 2, v->f3f, v->f3q) >> 1);
                s += ch >> 4;
                s = s > 16000 ? 16000 + ((s - 16000) >> 3) : s < -16000 ? -16000 + ((s + 16000) >> 3) : s;
            } else {                                /* brass: the lowpass opens with the attack */
                int32_t open = (v->env >> 9) + (int32_t)v->tone * 120;
                s = saw_blep(v->ph[0], inc, v->r);
                v->lp += mul15(s - v->lp, clampi(open >> 1, 1500, 30000));
                s = v->lp + (ch >> 3);
            }
            v->ph[0] += inc;
            out[i] += mul15(mul15(s, e0 + de * (int32_t)i), g);
        }
        v->nz_a = (int32_t)(((int64_t)v->nz_a * v->nz_k) >> 16);
        break;
    }
    case ENG_ORGAN: {
        uint32_t o = v->inst - I_ORGANETTO, rinc[9];
        int32_t chiff = v->nz_a >> 9, k, wob = 0;
        if (v->wob_amt) {
            v->wob_ph += v->wob_inc;
            wob = cents_oct((sin_ph(v->wob_ph) * v->wob_amt) >> 15);
        }
        for (k = 0; k < 9; k++)
            rinc[k] = wob ? inc_oct(v->inc[k], wob) : v->inc[k];
        for (i = 0; i < EMB_CTL; i++) {
            int32_t s = 0;
            for (k = 0; k < 9; k++)
                if (v->pa[k]) {
                    s += mul15(sin_ph(v->ph[k]), v->pa[k]);
                    v->ph[k] += rinc[k];
                }
            if (chiff)
                s += o == 3u ? mul15(noise(&v->nz), chiff) :
                               mul15(svf_bp(&v->f1, noise(&v->nz), v->f1f, v->f1q), chiff >> 1);
            out[i] += mul15(mul15(s >> 1, e0 + de * (int32_t)i), g);
        }
        v->nz_a = (int32_t)(((int64_t)v->nz_a * v->nz_k) >> 16);
        break;
    }
    case ENG_DRUM: {
        int32_t amp[4], n_a = v->nz_a >> 9, k;
        uint32_t pinc[4];
        int32_t drop = v->pdrop;                    /* Q16 octave: the membrane's pitch falls */
        for (k = 0; k < 4; k++) {
            amp[k] = v->pa[k] >> 9;
            pinc[k] = drop ? inc_oct(v->inc[k], drop) : v->inc[k];
        }
        v->pdrop = (int32_t)(((int64_t)v->pdrop * v->pdrop_k) >> 16);
        for (i = 0; i < EMB_CTL; i++) {
            int32_t s = 0, n = noise(&v->nz);
            for (k = 0; k < 4; k++)
                if (amp[k]) {
                    s += mul15(sin_ph(v->ph[k]), amp[k]);
                    v->ph[k] += pinc[k];
                }
            if (v->piece == DR_TAMB)                /* jingles: the partials ring-modulate the noise */
                s = mul15(s, n) * 3;
            s += mul15(svf_bp(&v->f1, n, v->f1f, v->f1q), n_a);
            out[i] += mul15(mul15(s, e0 + de * (int32_t)i), g);
        }
        for (k = 0; k < 4; k++)
            v->pa[k] = (int32_t)(((int64_t)v->pa[k] * v->pk[k]) >> 16);
        v->nz_a = (int32_t)(((int64_t)v->nz_a * v->nz_k) >> 16);
        break;
    }
    }
}

/* ------------------------------------------------------------------ sympathetic strings
 * Three undamped strings per string track, tuned to the root, fifth and octave of the playing key: what the
 * plucked strings play leaks into them (a harp's or a lute's body), they ring softly underneath. */
#define SYM_N 3u
#define SYM_LEN 1024u
static int16_t sym_buf[EMB_NTRK][SYM_N][SYM_LEN] EMB_POOL;
static struct { uint32_t d_q16[SYM_N]; uint16_t w; int32_t x1[SYM_N]; uint8_t root; } sym[EMB_NTRK];

static void sym_tune(uint32_t t, uint32_t root_note)
{
    static const uint8_t IV[SYM_N] = {0, 7, 12};
    uint32_t k;
    for (k = 0; k < SYM_N; k++)
        sym[t].d_q16[k] = ks_delay(pitch_inc((int32_t)(root_note + IV[k]) << 8, 0), 0x8000u);
    sym[t].root = (uint8_t)root_note;
}

static void sym_run(uint32_t t, int32_t *io, int32_t amount_q15)
{
    uint32_t i, k, w = sym[t].w;
    for (i = 0; i < EMB_CTL; i++) {
        int32_t in = io[i] >> 6, o = 0;
        for (k = 0; k < SYM_N; k++) {
            int16_t *b = sym_buf[t][k];
            uint32_t rp = (w << 16) - sym[t].d_q16[k], i0 = (rp >> 16) & (SYM_LEN - 1u), fr = rp & 0xFFFFu;
            int32_t a = b[i0], c = b[(i0 + 1u) & (SYM_LEN - 1u)], x = a + (int32_t)(((int64_t)(c - a) * fr) >> 16);
            int32_t y = mul15((x + sym[t].x1[k]) >> 1, 32700) + in;
            sym[t].x1[k] = x;
            b[w & (SYM_LEN - 1u)] = (int16_t)clampi(y, -32767, 32767);
            o += x;
        }
        w++;
        io[i] += mul15(o, amount_q15);
    }
    sym[t].w = (uint16_t)(w & (SYM_LEN - 1u));
}

/* ------------------------------------------------------------------ Leslie (per track, for the tonewheel) */
#define LES_LEN 256u
static struct {
    int16_t buf[LES_LEN];
    uint16_t w;
    uint32_t ph, inc, target;
} les[EMB_NTRK];
#define LES_SLOW 80800u                             /* 0.83 Hz per block: 0.83 * 2^32 * CTL / FS */
#define LES_FAST 652000u                            /* 6.7 Hz */

static void leslie_run(uint32_t t, const int32_t *in, int32_t *l, int32_t *r, int32_t gl, int32_t gr)
{
    uint32_t i, w = les[t].w;
    int32_t s, c;
    /* the rotor's inertia: about a second to spin up, two to slow */
    if (les[t].inc < les[t].target)
        les[t].inc += (les[t].target - les[t].inc) >> 7 | 1u;
    else
        les[t].inc -= (les[t].inc - les[t].target) >> 8;
    les[t].ph += les[t].inc;
    s = sin_ph(les[t].ph);
    c = sin_ph(les[t].ph + 0x40000000u);
    for (i = 0; i < EMB_CTL; i++) {
        uint32_t dl = 64u + (uint32_t)((s * 12) >> 15), d2 = 64u - (uint32_t)((s * 12) >> 15);
        int32_t a, b;
        les[t].buf[w & (LES_LEN - 1u)] = (int16_t)clampi(in[i], -32767, 32767);
        a = les[t].buf[(w - dl) & (LES_LEN - 1u)];
        b = les[t].buf[(w - d2) & (LES_LEN - 1u)];
        w++;
        l[i] += mul15(mul15(a, 26000 + (c >> 3)), gl);   /* horn toward the left mic, then the right */
        r[i] += mul15(mul15(b, 26000 - (c >> 3)), gr);
    }
    les[t].w = (uint16_t)(w & (LES_LEN - 1u));
}

/* ------------------------------------------------------------------ SANCTUARY reverb
 * A stone-temple space: pre-delay, four input allpass diffusers, a 4-line feedback delay network
 * (Hadamard mixing, damping in every line, two lines slowly modulated) and decorrelated stereo taps.
 * RT60 0.5 .. 12 s; SIZE scales the lines 40 .. 100 %. */
#define RV_N 4u
#define RV_MAX 4096u
#define RV_PRE 2048u
static const uint16_t RV_LEN[RV_N] = {2203, 2647, 3119, 3571};
static const uint16_t AP_LEN[4] = {142, 107, 379, 277};
static int16_t rv_line[RV_N][RV_MAX] EMB_POOL;
static int16_t rv_ap[4][512] EMB_POOL;
static int16_t rv_pre[RV_PRE] EMB_POOL;
static struct {
    uint32_t len[RV_N], g[RV_N], w, ap_w[4], pre_w, pre_d, mod_ph;
    int32_t lp[RV_N], damp, mix, mix_t;
} rv;

static void rv_set(uint32_t size, uint32_t decay, uint32_t damp, uint32_t mix, uint32_t swell)
{
    uint32_t k, rt_ms;
    if (swell) {
        decay = decay + 30u > 127u ? 127u : decay + 30u;
        mix = mix + 40u > 127u ? 127u : mix + 40u;
        size = 127u;
    }
    rt_ms = (uint32_t)(((uint64_t)500u * exp2_frac((decay * 2374u) & 0xFFFFu)) >> 26) >> (4u - ((decay * 2374u) >> 16));
    for (k = 0; k < RV_N; k++) {
        uint32_t len = (RV_LEN[k] * (102u + size)) >> 8;   /* 40 .. 90 % .. of the buffer */
        rv.len[k] = len;
        rv.g[k] = exp2_neg_q16(emb_udiv(14811u * len, rt_ms));   /* 2^(-9.966 len / (FS * RT)) */
    }
    rv.damp = 6000 + (int32_t)(127u - damp) * 200;
    rv.mix_t = (int32_t)mix * 200;
    rv.pre_d = 200u + size * 8u;
}

static void rv_run(const int32_t *in_l, const int32_t *in_r, int32_t *out_l, int32_t *out_r)
{
    uint32_t i, k, w = rv.w;
    int32_t mod;
    rv.mod_ph += 30000u * EMB_CTL;                  /* ~0.3 Hz */
    mod = sin_ph(rv.mod_ph) >> 11;                  /* +-16 samples Q? -> +-15 */
    rv.mix += (rv.mix_t - rv.mix) >> 6;
    for (i = 0; i < EMB_CTL; i++) {
        int32_t x = (in_l[i] + in_r[i]) >> 2, y[RV_N], h0, h1, h2, h3;
        rv_pre[rv.pre_w & (RV_PRE - 1u)] = (int16_t)clampi(x, -32767, 32767);
        x = rv_pre[(rv.pre_w - rv.pre_d) & (RV_PRE - 1u)];
        rv.pre_w++;
        for (k = 0; k < 4u; k++) {                  /* diffusion: allpass, g 0.6 */
            uint32_t wp = rv.ap_w[k];
            int32_t d = rv_ap[k][(wp + 512u - AP_LEN[k]) & 511u], v = x - mul15(d, 19661);
            rv_ap[k][wp & 511u] = (int16_t)clampi(v, -32767, 32767);
            x = d + mul15(v, 19661);
            rv.ap_w[k] = (wp + 1u) & 511u;
        }
        for (k = 0; k < RV_N; k++) {
            uint32_t off = rv.len[k] + (k & 1u ? (uint32_t)(mod + 16) : 0u);
            y[k] = rv_line[k][(w - off) & (RV_MAX - 1u)];
            rv.lp[k] += mul15(y[k] - rv.lp[k], rv.damp);
            y[k] = (int32_t)(((int64_t)rv.lp[k] * rv.g[k]) >> 16);
        }
        h0 = (y[0] + y[1] + y[2] + y[3]) >> 1;      /* Hadamard / 2: lossless mixing */
        h1 = (y[0] - y[1] + y[2] - y[3]) >> 1;
        h2 = (y[0] + y[1] - y[2] - y[3]) >> 1;
        h3 = (y[0] - y[1] - y[2] + y[3]) >> 1;
        rv_line[0][w & (RV_MAX - 1u)] = (int16_t)clampi(h0 + x, -32767, 32767);
        rv_line[1][w & (RV_MAX - 1u)] = (int16_t)clampi(h1 + x, -32767, 32767);
        rv_line[2][w & (RV_MAX - 1u)] = (int16_t)clampi(h2 - x, -32767, 32767);
        rv_line[3][w & (RV_MAX - 1u)] = (int16_t)clampi(h3 - x, -32767, 32767);
        w++;
        out_l[i] += mul15(y[0] + y[2] - (y[1] >> 1), rv.mix);
        out_r[i] += mul15(y[1] + y[3] - (y[2] >> 1), rv.mix);
    }
    rv.w = w & (RV_MAX - 1u);
}
