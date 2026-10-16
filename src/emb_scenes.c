/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: the sixteen factory Encounter scenes and the track defaults.
 *
 * A scene's tracks are written as pattern strings, one character a sixteenth step, lanes split by '|'
 * (a lane per chord tone / drum layer, up to EMB_NNOTE):
 *   melodic  '0'..'9' scale degree above the track's root (7 = the octave), 'a'..'g' degrees -7..-1,
 *            '.' rest, '-' tie (the note before lasts a step longer), '~' hold until the lane's next note
 *   drums    B bodhran  F frame drum  T tambourine  A anvil  P timpani  W war drum  C clack  G gong
 *            (lowercase: a ghost note), '.' rest
 * The white keys run left to right from rest to climax: keys 1-4 REST, 5-8 EXPLORE, 9-12 COMBAT,
 * 13-16 CLIMAX, so a bard finds them by touch. */

typedef struct {
    uint8_t inst, oct, level, chr;                  /* oct: the root's octave (3 = the octave of C3) */
    const char *pat;
} tdef_t;
typedef struct {
    const char *name;
    uint8_t bpm, root, mode, temper, swing;
    tdef_t t[EMB_NTRK];
} sdef_t;

enum { C_ = 0, D_ = 2, E_ = 4, F_ = 5, G_ = 7, A_ = 9, B_ = 11 };
static const sdef_t SCENES[EMB_NSLOT] = {
    /* ---- REST ---- */
    {"TAVERN", 96, D_, M_DORIAN, T_PYTH, 3,
     {{I_TAVERN_KIT, 0, 96, 0, "B.....b.B...b...|....T.......T.t."},
      {I_GURDY, 2, 64, 0, "0~..............|4~.............."},
      {I_HARP, 3, 100, 40, "0.2.4.7.4.2.0.2.1.3.5.8.5.3.1.3."},
      {I_RECORDER, 4, 82, 30, "................4-3-2-0---......"}}},
    {"CAMPFIRE", 72, A_, M_AEOLIAN, T_PYTH, 2,
     {{I_TAVERN_KIT, 0, 80, 0, "B.......b.......|..........c....."},
      {I_CELLO, 2, 56, 0, "0~..............|4~.............."},
      {I_LUTE, 3, 100, 60, "0.4.7.4.0.4.7.4.b.3.6.3.b.3.6.3."},
      {I_PANFLUTE, 4, 70, 50, "........................2-1-0---"}}},
    {"SANCTUARY", 60, D_, M_DORIAN, T_JUST, 0,
     {{I_RITUAL_KIT, 0, 60, 0, "P...............|................"},
      {I_CHOIR, 3, 84, 20, "0~..............3~..............|4~..............5~.............."},
      {I_HARP, 4, 70, 30, "0...4...7...4...2...5...9...5..."},
      {I_POSITIVE, 3, 62, 0, "7~..............8~..............|2~..............3~.............."}}},
    {"LAMENT", 64, E_, M_AEOLIAN, T_MEAN, 0,
     {{I_RITUAL_KIT, 0, 50, 0, "F...............|................"},
      {I_CELLO, 2, 70, 0, "0~..............e~..............|4~..............3~.............."},
      {I_KOTO, 3, 80, 80, "0.....4.....7...2.....5.....9..."},
      {I_PANFLUTE, 4, 80, 70, "4---3-2-1---0-------2---1-0-----"}}},
    /* ---- EXPLORE ---- */
    {"WILDERNESS", 84, E_, M_DORIAN, T_PYTH, 0,
     {{I_TAVERN_KIT, 0, 64, 0, "F.......f.......F.......f...f..."},
      {I_BAGPIPE, 2, 46, 0, "0~..............|4~.............."},
      {I_PSALTERY, 4, 60, 40, "..............7.........4.......|................................"},
      {I_PANFLUTE, 4, 84, 60, "4---2-......................4-7-------..........................."}}},
    {"FEY FOREST", 100, G_, M_LYDIAN, T_JUST, 4,
     {{I_TAVERN_KIT, 0, 64, 0, "F...f...F...f.f.|..T...T...T...T."},
      {I_CHOIR, 3, 58, 60, "0~..............|4~.............."},
      {I_PSALTERY, 4, 84, 30, "0247024702470247365836583658a365"},
      {I_RECORDER, 5, 70, 20, "4-..3-..4-6-7---................"}}},
    {"DUNGEON", 76, C_, M_PHRYGIAN, T_MEAN, 0,
     {{I_WAR_KIT, 0, 80, 0, "W.......w.....W.|....C.........c.|..........A....."},
      {I_CELLO, 2, 72, 0, "0~..............1~..............|4~..............4~.............."},
      {I_KOTO, 3, 62, 90, "......1.....0...........4.......|................................"},
      {I_ORGANETTO, 4, 50, 0, "................0-1-0-6---......"}}},
    {"SEA ROAD", 112, D_, M_MIXOLYDIAN, T_PYTH, 5,
     {{I_TAVERN_KIT, 0, 90, 0, "B..b..B..b..B.bb|...T.....T.....T"},
      {I_GURDY, 2, 60, 70, "0~..............|4~.............."},
      {I_NYLON, 3, 80, 50, "0.47.40.47.40.476.36.63.36.63.36"},
      {I_SHAWM, 4, 76, 40, "4-5-6-7-6-4-5---3-4-5-6-5-3-4---"}}},
    /* ---- COMBAT ---- */
    {"SKIRMISH", 132, D_, M_AEOLIAN, T_EQUAL, 0,
     {{I_WAR_KIT, 0, 110, 0, "W...W...W...W.W.|..c...c...c...cc|B.b.B.b.B.b.B.bb"},
      {I_CELLO, 1, 92, 0, "0.0.0.0.0.0.1.0.0.0.0.0.6.6.5.4."},
      {I_NYLON, 3, 50, 30, "................................"},
      {I_BRASS, 3, 70, 0, "........................0.......|........................4......."}}},
    {"BATTLE", 144, E_, M_PHRYGIAN, T_EQUAL, 0,
     {{I_WAR_KIT, 0, 120, 0, "W.wW.wW.W.wW.wW.|....C.......C...|T.T.T.T.T.T.T.TT"},
      {I_GURDY, 2, 84, 110, "0~..............|4~.............."},
      {I_CELLO, 2, 90, 0, "0.0.1.0.0.0.1.0.3.3.1.0.e.e.0..."},
      {I_BRASS, 3, 82, 0, "0---..0-1-0-....4---..4-5-4-....|4---..4-5-4-....7---..7-8-7-...."}}},
    {"AMBUSH", 120, C_, M_HARMONIC, T_EQUAL, 0,
     {{I_WAR_KIT, 0, 80, 0, "....c...c.c.....|F.......f.....f.|................"},
      {I_CELLO, 1, 70, 0, "0...0...0...0.1.0...0...0...6.5."},
      {I_KOTO, 3, 72, 100, "......4.......1.......4.....6..."},
      {I_SHAWM, 4, 46, 30, "................................"}}},
    {"SIEGE", 96, A_, M_AEOLIAN, T_EQUAL, 0,
     {{I_WAR_KIT, 0, 120, 0, "W.......W.W.....|P...........P...|....C.......C..."},
      {I_CHOIR, 2, 72, 30, "0~..............e~..............|4~..............3~.............."},
      {I_CATHEDRAL, 2, 78, 0, "0~......f~......d~......e~......|4~......3~......1~......2~......|7~......5~......5~......6~......"},
      {I_BRASS, 3, 76, 0, "0-....0-0-4-....3-....3-3-1-...."}}},
    /* ---- CLIMAX ---- */
    {"BOSS", 150, D_, M_HARMONIC, T_EQUAL, 0,
     {{I_WAR_KIT, 0, 124, 0, "W.WW.WW.W.WW.WW.|T.T.T.T.T.T.T.T.|....P.......P..."},
      {I_CHOIR, 3, 82, 10, "0~..............1~..............|4~..............4~.............."},
      {I_CATHEDRAL, 2, 90, 0, "0~......1~......f~......6~......|4~......4~......4~......3~......|7~......8~......7~......d~......"},
      {I_BRASS, 3, 92, 0, "0-..0-..0-0-0-..1-..1-..1-1-1-..|4-..4-..4-4-4-..4-..4-..4-4-4-.."}}},
    {"LAST STAND", 128, E_, M_AEOLIAN, T_PYTH, 0,
     {{I_WAR_KIT, 0, 120, 0, "W..W..W.W..W..W.|..T...T...T...TT|B.b.B.b.B.b.B.b."},
      {I_GURDY, 2, 86, 120, "0~..............|4~.............."},
      {I_CATHEDRAL, 3, 70, 0, "0~..............5~..............|4~..............9~.............."},
      {I_SHAWM, 4, 84, 50, "4-3-4-5-4-3-2-0-2-3-4-3-2-1-0---"}}},
    {"VICTORY", 120, D_, M_IONIAN, T_JUST, 2,
     {{I_TAVERN_KIT, 0, 100, 0, "B...B...B.B.B...|T.T.T.T.T.T.T.TT"},
      {I_POSITIVE, 3, 66, 0, "0~..............3~..............|4~..............5~..............|7~..............7~.............."},
      {I_HARP, 4, 76, 40, "0247024735873587"},
      {I_BRASS, 4, 92, 0, "0-0-0-4---..2-4-7-------3-4-5-4-"}}},
    {"REQUIEM", 56, D_, M_AEOLIAN, T_JUST, 0,
     {{I_RITUAL_KIT, 0, 70, 0, "G...............................|................................"},
      {I_CHOIR, 3, 80, 90, "0~..............................|4~..............................|7~.............................."},
      {I_HARP, 4, 40, 20, "................................"},
      {I_CATHEDRAL, 2, 44, 0, "0~..............................|7~.............................."}}},
};

const char *const emb_scene_name[EMB_NSLOT] = {
    "TAVERN", "CAMPFIRE", "SANCTUARY", "LAMENT", "WILDERNESS", "FEY FOREST", "DUNGEON", "SEA ROAD",
    "SKIRMISH", "BATTLE", "AMBUSH", "SIEGE", "BOSS", "LAST STAND", "VICTORY", "REQUIEM"};
const char *const emb_mod_name[12] = {"PHRYGIAN", "HOME KEY", "HARMONIC", "TRIUMPH", "KEY UP", "KEY DOWN",
                                      "DOUBLE TIME", "HALF TIME", "BREAKDOWN", "FULL HOST", "STINGER", "SWELL"};

void emb_track_defaults(emb_track_t *t, uint32_t inst)
{
    uint32_t e = INST_ENG[inst], k;
    for (k = 0; k < sizeof *t; k++)
        ((uint8_t *)t)[k] = 0;
    for (k = 0; k < EMB_NSTEP; k++) {
        t->s[k].lock = 255;
        t->s[k].gate = 4;
        t->s[k].vel = 100;
    }
    t->inst = (uint8_t)inst;
    t->len = 16;
    t->level = 100;
    t->pan = 64;
    t->tone = 64;
    t->chr = 40;
    t->rev = 50;
    t->att = 0;
    t->dec = 60;
    t->sus = 110;
    t->rel = 40;
    t->draw[0] = 6;
    t->draw[1] = 8;
    t->draw[2] = 4;
    t->draw[3] = 2;
    switch (e) {
    case ENG_DRUM: t->sus = 0; t->rev = 32; t->level = 110; break;
    case ENG_PLUCK: t->dec = 90; t->rel = 50; t->tone = 70; break;
    case ENG_CHOIR: t->att = 70; t->dec = 80; t->rel = 80; t->rev = 85; t->chr = 20; break;
    case ENG_BOW: t->att = 34; t->rel = 48; t->chr = inst == I_GURDY ? 60 : 30; break;
    case ENG_WIND: t->att = 24; t->rel = 38; t->rev = 58; break;
    case ENG_ORGAN:
        t->att = 6; t->sus = 127; t->rel = inst == I_CATHEDRAL ? 72 : 34;
        t->rev = inst == I_CATHEDRAL ? 96 : inst == I_TONEWHEEL ? 26 : 64;
        break;
    }
}

void emb_song_clear(emb_song_t *s, const char *name)
{
    uint32_t k;
    static const uint8_t DEF[EMB_NTRK] = {I_TAVERN_KIT, I_GURDY, I_HARP, I_RECORDER};
    for (k = 0; k < sizeof *s; k++)
        ((uint8_t *)s)[k] = 0;
    s->magic = EMB_SONG_MAGIC;
    for (k = 0; k < sizeof s->name - 1u && name[k]; k++)
        s->name[k] = name[k];
    s->bpm = 96;
    s->root = D_;
    s->mode = M_DORIAN;                             /* locked to the modes by default (MANUAL: the Modes) */
    s->temper = T_PYTH;
    s->vib_rate = 48;
    s->vib_depth = 10;
    for (k = 0; k < EMB_NTRK; k++)
        emb_track_defaults(&s->t[k], DEF[k]);
    s->t[2].pan = 46;
    s->t[3].pan = 82;
}

static uint32_t drum_piece(char c)
{
    static const char P[] = "BFTAPWCG";
    uint32_t k;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    for (k = 0; k < 8u; k++)
        if (P[k] == c)
            return k + 1u;
    return 0;
}

/* one lane of a pattern into the notes' column `lane` */
static void parse_lane(emb_track_t *t, const char *p, uint32_t n, uint32_t lane, int base, uint32_t mode)
{
    uint32_t i, last = 0xFFu;
    for (i = 0; i < n && i < EMB_NSTEP; i++) {
        char c = p[i];
        emb_step_t *s = &t->s[i];
        int deg = -100;
        if (c == '-' && last != 0xFFu) {
            if (t->s[last].gate != 255u)
                t->s[last].gate = (uint8_t)(t->s[last].gate + 4u);
            continue;
        }
        if (c == '~' && last != 0xFFu) {
            t->s[last].gate = 255;
            continue;
        }
        if (t->inst < I_FIRST_MELODIC) {
            uint32_t pc = drum_piece(c);
            if (pc) {
                s->note[lane] = (uint8_t)pc;
                if (c >= 'a' && c <= 'z')
                    s->vel = 60;
                else if (s->vel < 100u || lane == 0u)
                    s->vel = 112;
                last = i;
            }
            continue;
        }
        if (c >= '0' && c <= '9')
            deg = c - '0';
        else if (c >= 'a' && c <= 'g')
            deg = c - 'a' - 7;
        if (deg > -100) {
            int o = 0, d = deg;
            while (d < 0) {
                d += 7;
                o--;
            }
            while (d >= 7) {
                d -= 7;
                o++;
            }
            s->note[lane] = (uint8_t)clampi(base + o * 12 + emb_scale[mode][d], 1, 127);
            s->gate = 4;
            last = i;
        }
    }
}

void emb_scene_factory(uint32_t slot, emb_song_t *s)
{
    const sdef_t *d = &SCENES[slot & 15u];
    uint32_t k;
    emb_song_clear(s, d->name);
    s->bpm = d->bpm;
    s->root = d->root;
    s->mode = d->mode;
    s->temper = d->temper;

    s->swing = d->swing;
    for (k = 0; k < EMB_NTRK; k++) {
        const tdef_t *td = &d->t[k];
        emb_track_t *t = &s->t[k];
        const char *p = td->pat;
        uint32_t lane = 0, len = 0;
        emb_track_defaults(t, td->inst);
        t->level = td->level;
        if (td->chr)
            t->chr = td->chr;
        t->pan = k == 2u ? 46 : k == 3u ? 82 : 64;
        while (*p && lane < EMB_NNOTE) {
            uint32_t n = 0;
            while (p[n] && p[n] != '|')
                n++;
            parse_lane(t, p, n, lane, 12 * (td->oct + 1) + d->root, d->mode);
            if (n > len)
                len = n;
            p += n;
            if (*p == '|')
                p++;
            lane++;
        }
        t->len = (uint8_t)(len ? (len > EMB_NSTEP ? EMB_NSTEP : len) : 16u);
    }
    s->sum = emb_sum(s, (uint32_t)((const uint8_t *)&s->sum - (const uint8_t *)s));
}
