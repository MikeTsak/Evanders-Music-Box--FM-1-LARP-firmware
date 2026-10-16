/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box on a computer: the self-test, a WAV renderer and screenshots.
 *   host                 run the self-test (exit 0 = every check passed)
 *   host wav SLOT SEC F  render an Encounter scene (0..15) to a WAV file
 *   host shots DIR       write PPM screenshots (the boot, the pages, the Encounter screen)
 * Build: tools/host_build.py (zig cc or any C compiler). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../emb.c"

int emb_hal_store(uint32_t slot, const void *p, uint32_t n) { (void)slot; (void)p; (void)n; return 0; }
int emb_hal_load(uint32_t slot, void *p, uint32_t n) { (void)slot; (void)p; (void)n; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t buf[2 * 4096];
static uint32_t clock_ms;
static void run(uint32_t frames, int32_t *peak)     /* render, advance the UI clock */
{
    uint32_t done = 0, i;
    while (done < frames) {
        uint32_t n = frames - done > 1024u ? 1024u : frames - done;
        n -= n % EMB_CTL;
        if (!n)
            break;
        emb_render(buf, n);
        for (i = 0; i < 2u * n; i++)
            if (peak && abs(buf[i]) > *peak)
                *peak = abs(buf[i]);
        done += n;
        clock_ms += n * 1000u / EMB_FS;
        emb_tick(clock_ms);
    }
}
static void press(uint32_t b) { emb_button(b, 1); emb_button(b, 0); }

static void test_math(void)
{
    uint32_t i, inc = pitch_inc(69 << 8, 0), want = 42852281u;
    CHECK(emb_sine[256] > 32700 && emb_sine[768] < -32700 && abs(emb_sine[0]) < 2, "sine table");
    CHECK(inc > want - want / 2000u && inc < want + want / 2000u, "A4 increment %u, want %u", inc, want);
    CHECK(abs((int)(pitch_inc(81 << 8, 0) / 2u) - (int)inc) < (int)(inc / 1000u) + 2, "octave");
    for (i = 1; i < 2000u; i++) {
        uint32_t a = i * 2654435761u, b = (i * 40503u) | 1u;
        if (emb_udiv(a, b) != a / b) {
            CHECK(0, "udiv %u / %u", a, b);
            break;
        }
    }
    CHECK(pow48_div(42852281u) == (uint32_t)(((uint64_t)1 << 48) / 42852281u) && pow48_div(4200000u) == (uint32_t)(((uint64_t)1 << 48) / 4200000u), "pow48_div");
    CHECK(time_ms(0) == 2u && time_ms(127) > 9000u && time_ms(127) < 14000u, "time_ms %u %u", time_ms(0), time_ms(127));
    CHECK(exp2_neg_q16(0) == 65536u && exp2_neg_q16(65536) == 32768u, "exp2");
    CHECK(emb_snap(61, 2, M_DORIAN) == 60, "snap C# in D dorian -> C");   /* D E F G A B C */
    CHECK(emb_in_scale(62, 2, M_DORIAN) && !emb_in_scale(63, 2, M_DORIAN), "in scale");
    CHECK(remap(65, 2, M_DORIAN, 2, M_PHRYGIAN) == 65 && remap(64, 2, M_DORIAN, 2, M_PHRYGIAN) == 63,
          "dorian E -> phrygian Eb");
    CHECK(remap(62, 2, M_DORIAN, 4, M_DORIAN) == 64, "key up");
    CHECK(temper_cents(69, 62, T_PYTH) == 2 && temper_cents(66, 62, T_JUST) == -13, "temperaments");
}

static void test_scenes(void)
{
    uint32_t k, t;
    for (k = 0; k < EMB_NSLOT; k++) {
        uint32_t notes = 0;
        CHECK(song_ok(&emb_slot[k]), "scene %u checksum", k);
        CHECK(!strcmp(emb_slot[k].name, emb_scene_name[k]), "scene %u name %s", k, emb_slot[k].name);
        for (t = 0; t < EMB_NTRK; t++) {
            uint32_t s;
            CHECK(emb_slot[k].t[t].len >= 1u && emb_slot[k].t[t].len <= EMB_NSTEP, "scene %u track %u len", k, t);
            for (s = 0; s < EMB_NSTEP; s++)
                notes += emb_slot[k].t[t].s[s].note[0] != 0;
        }
        CHECK(notes >= 3u, "scene %u has %u notes", k, notes);
    }
}

static void test_play(void)
{
    uint32_t k;
    for (k = 0; k < EMB_NSLOT; k++) {               /* every scene sounds, none clips hard */
        int32_t peak = 0;
        emb_post(C_PANIC, 0, 0, 0, 0);
        run(EMB_CTL * 4u, 0);
        emb_post(C_QUEUE, k, 0, 0, 0);
        run(EMB_FS * 3u, &peak);
        CHECK(peak > 1500, "scene %u (%s) is too quiet: %d", k, emb_scene_name[k], peak);
        CHECK(peak < 32000, "scene %u (%s) hits the rail: %d", k, emb_scene_name[k], peak);
        CHECK(emb_st.deck_slot[emb_st.active] == k, "scene %u is not playing", k);
        printf("  scene %2u %-11s peak %5d voices %u\n", k + 1u, emb_scene_name[k], peak, emb_st.voices);
    }
}

static void test_crossfade(void)
{
    uint32_t n = 0;
    emb_set.quant = 1;                              /* the next bar */
    emb_post(C_PANIC, 0, 0, 0, 0);
    emb_post(C_QUEUE, 0, 0, 0, 0);
    run(EMB_FS, 0);
    emb_post(C_QUEUE, 9, 0, 0, 0);
    run(EMB_CTL * 2u, 0);
    CHECK(emb_st.queued == 10u && emb_st.deck_slot[emb_st.active] == 0u, "queued, still playing the tavern");
    while (emb_st.deck_slot[emb_st.active] != 9u && n++ < 2000u)
        run(EMB_CTL * 8u, 0);
    CHECK(emb_st.deck_slot[emb_st.active] == 9u, "the battle took over");
    CHECK((deck[emb_st.active].gstep & 15u) <= 1u, "on the bar (step %u)", deck[emb_st.active].gstep);
    CHECK(emb_st.fading, "the tavern fades under it");
    run(EMB_FS * 3u, 0);
    CHECK(!emb_st.fading, "and is gone");
}

static void test_microtiming(void)
{
    uint32_t n = 0, started = 0;
    emb_post(C_PANIC, 0, 0, 0, 0);
    run(EMB_FS / 5u, 0);                            /* the released voices die away */
    emb_song_clear(&emb_bench, "TEST");
    emb_bench.bpm = 120;                            /* a step = 5512.5 samples, a tick ~229.7 */
    emb_bench.t[2].s[1].note[0] = 62;
    emb_bench.t[2].s[1].micro = 6;                  /* a quarter step late */
    emb_post(C_PLAY, 255, 0, 0, 0);
    while (n < EMB_FS && !started) {
        run(EMB_CTL, 0);
        n += EMB_CTL;
        started = emb_st.voices > 0u;
    }
    /* step 1 sounds at 1.25 steps: 6890 samples (block granularity) */
    CHECK(started && n > 6890u - 300u && n < 6890u + 300u, "microtimed note at %u, want ~6890", n);
    emb_post(C_STOP, 0, 0, 0, 0);
    run(EMB_CTL * 2u, 0);
}

static void test_purge_undo(void)
{
    emb_bench.t[1].s[3].note[0] = 50;
    emb_post(C_PURGE, 1, 0, 0, 0);
    run(EMB_CTL, 0);
    CHECK(emb_bench.t[1].s[3].note[0] == 0, "purged");
    emb_post(C_PURGE, 1, 1, 0, 0);
    run(EMB_CTL, 0);
    CHECK(emb_bench.t[1].s[3].note[0] == 50, "undone");
}

static void test_ui(void)
{
    uint8_t lv[EMB_NLED];
    static uint16_t fb[EMB_W * EMB_H];
    uint32_t dirty[8];
    run(EMB_FS * 4u, 0);                            /* past the boot animation */
    CHECK(!emb_booting(), "boot finished");
    CHECK(ui.mode == 0u, "starts in Bardic Crafting");
    emb_button(B_HOME, 1);
    emb_button(B_PLAY, 1);
    emb_button(B_PLAY, 0);
    emb_button(B_HOME, 0);
    CHECK(ui.mode == 1u && ui.page == P_HOME, "HOME + PLAY: Encounter");
    emb_key(emb_key_of_white[12], 1, 100);          /* key 13: BOSS */
    emb_key(emb_key_of_white[12], 0, 0);
    run(EMB_CTL * 4u, 0);
    CHECK(emb_st.deck_slot[emb_st.active] == 12u || emb_st.queued == 13u, "key 13 cues the boss");
    emb_leds(lv);
    CHECK(lv[B_COUNT + emb_key_of_white[12]] == LED_LIT || emb_st.queued, "its key is lit");
    CHECK(emb_draw(fb, dirty), "the encounter screen draws");
    emb_button(B_HOME, 1);                          /* hold HOME + REC: silence after 0.4 s */
    emb_button(B_REC, 1);
    run(EMB_FS / 4u, 0);
    CHECK(!ui.panic_fired, "not yet");
    run(EMB_FS / 4u, 0);
    CHECK(ui.panic_fired, "fired");
    emb_button(B_REC, 0);
    emb_button(B_HOME, 0);
    run(EMB_CTL * 2u, 0);
    CHECK(!emb_st.playing, "silence");
    emb_button(B_PLAY, 1);                          /* back to Bardic: HOME + PLAY, either order */
    emb_button(B_HOME, 1);
    emb_button(B_HOME, 0);
    emb_button(B_PLAY, 0);
    CHECK(ui.mode == 0u, "back to Bardic");
    CHECK(!emb_st.playing, "the PLAY of the chord did not start the transport");
    press(B_SEQ);
    CHECK(ui.page == P_SEQ, "SEQ page");
    emb_key(emb_key_of_white[0], 1, 100);
    emb_key(emb_key_of_white[0], 0, 0);
    CHECK(emb_bench.t[ui.trk].s[0].note[0] != 0, "a tapped step is set");
    emb_button(B_EDIT, 1);
    emb_button(B_GLO, 1);
    emb_button(B_GLO, 0);
    emb_button(B_EDIT, 0);
    CHECK(emb_bench.t[0].inst == I_TONEWHEEL, "EDIT + GLO: tonewheel on track 1 (lead)");
    press(B_SEL);
    CHECK(ui.trk == 1u, "SEL: track 2");
    press(B_SEL);
    CHECK(ui.trk == 2u, "SEL: track 3");
    press(B_SEL);
    CHECK(ui.trk == 3u, "SEL: track 4 (drums)");
    emb_button(B_EDIT, 1);
    emb_button(B_GLO, 1);
    emb_button(B_GLO, 0);
    emb_button(B_EDIT, 0);
    CHECK(emb_bench.t[3].inst == I_TAVERN_KIT, "drums cannot switch to tonewheel");
    {
        uint32_t p;
        for (p = 0; p < 7u; p++) {                  /* every page draws */
            ui.page = (uint8_t)p;
            emb_draw(fb, dirty);
        }
    }
}

static void test_sysex(void)
{
    uint8_t in[700], out[700];
    uint32_t c, n;
    emb_song_t before = emb_slot[5];
    for (c = 0; c < SX_NCHUNK; c++) {               /* read slot 3, write it into slot 5 */
        in[0] = 0x7D; in[1] = 0x45; in[2] = 0x4D; in[3] = 2; in[4] = 3; in[5] = (uint8_t)c;
        n = emb_sysex(in, 6, out);
        CHECK(n > 6u, "get chunk %u", c);
        memcpy(in + 6, out + 6, n - 6u);
        in[3] = 3;
        in[4] = 5;
        n = emb_sysex(in, n, out);
        CHECK(n == 7u && out[6] == 0u, "put chunk %u: status %u", c, out[6]);
    }
    CHECK(!memcmp(&emb_slot[5], &emb_slot[3], sizeof emb_slot[3]), "slot 5 = slot 3");
    in[3] = 4;
    in[4] = 5;
    emb_sysex(in, 5, out);
    CHECK(!memcmp(&emb_slot[5], &before, sizeof before), "factory restore");
}

static void put16(FILE *f, uint32_t v) { fputc((int)(v & 255u), f); fputc((int)(v >> 8), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFFu); put16(f, v >> 16); }

static int wav(uint32_t slot, uint32_t sec, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t frames = sec * EMB_FS, done = 0, i;
    if (!f)
        return 1;
    fwrite("RIFF", 1, 4, f); put32(f, 36u + frames * 4u); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, EMB_FS); put32(f, EMB_FS * 4u); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 4u);
    emb_post(C_QUEUE, slot, 0, 0, 0);
    while (done < frames) {
        emb_render(buf, 1024);
        for (i = 0; i < 2048u; i++)
            put16(f, (uint32_t)(uint16_t)(int16_t)clampi(buf[i], -32767, 32767));
        done += 1024u;
    }
    fclose(f);
    printf("%s: %s, %u s\n", path, emb_scene_name[slot & 15u], sec);
    return 0;
}

static void ppm(const char *dir, const char *name, const uint16_t *fb)
{
    char path[512];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", EMB_W, EMB_H);
    for (i = 0; i < EMB_W * EMB_H; i++) {
        uint16_t c = fb[i];
        fputc((c >> 8) & 0xF8, f);
        fputc((c >> 3) & 0xFC, f);
        fputc((c << 3) & 0xF8, f);
    }
    fclose(f);
}

/* the boot animation runs its length whatever the first tick's clock says (an even ms once ended it at once) */
static void test_boot(void)
{
    emb_init(2);
    emb_tick(1000);
    CHECK(emb_booting(), "booting after an even first tick");
    emb_tick(1000 + BOOT_MS / 2u);
    CHECK(emb_booting(), "still booting halfway");
    emb_tick(1001 + BOOT_MS);
    CHECK(!emb_booting(), "boot over after BOOT_MS");
}

/* Encounter: the group keys, the sharps' variations, the held group key, the modifier keys */
static void test_encounter_keys(void)
{
    uint32_t s, k, seen = 0;
    for (k = 0; k < EMB_NKEY; k++) {
        int sl = enc_slot(k);
        if (sl >= 0) {
            CHECK(!((seen >> sl) & 1u), "slot %d reached by two keys", sl);
            seen |= 1u << sl;
        }
    }
    CHECK(seen == 0x7FFFu, "keys reach slots 0..14 (got %04X); 15 is the CLIMAX key held", seen);
    for (s = 0; s < 4u; s++)
        CHECK(enc_slot((uint32_t)CAT_KEY[s]) == (int)s * 4 && emb_white_of[CAT_KEY[s]] == (int)s * 4,
              "group %u: white key %u", s, s * 4u + 1u);
    CHECK(enc_slot(1) == 1 && enc_slot(3) == 2 && enc_slot(5) == 3, "REST's sharps: II III IV");
    CHECK(enc_slot(13) == 7 && enc_slot(20) == 11 && enc_slot(25) == 14, "the last sharp of each row");
    ui.mode = 1;
    emb_post(C_PANIC, 0, 0, 0, 0);
    run(EMB_CTL * 2u, 0);
    emb_key(21, 1, 100);                            /* hold CLIMAX: BOSS at once, REQUIEM after 0.6 s */
    run(EMB_CTL * 4u, 0);
    CHECK(emb_st.deck_slot[emb_st.active] == 12u, "CLIMAX key: BOSS");
    run(EMB_FS * 7u / 10u, 0);
    emb_key(21, 0, 0);
    CHECK(emb_st.queued == 16u, "held: REQUIEM queued (%u)", emb_st.queued);
    emb_key(22, 1, 100);                            /* D#5: CLIMAX II */
    emb_key(22, 0, 0);
    run(EMB_CTL * 2u, 0);
    CHECK(emb_st.queued == 14u, "D#5 queues LAST STAND (%u)", emb_st.queued);
    emb_key(emb_key_of_white[1], 1, 100);           /* white 2: PHRYGIAN, waiting for LAST STAND, then on it */
    emb_key(emb_key_of_white[1], 0, 0);
    for (k = 0; k < 400u && emb_st.deck_slot[emb_st.active] != 13u; k++)
        run(EMB_CTL * 4u, 0);
    CHECK(emb_st.deck_slot[emb_st.active] == 13u, "LAST STAND took over");
    CHECK(emb_st.mod_mode == M_PHRYGIAN, "white 2: Phrygian, on the new scene");
    {
        uint8_t lv[EMB_NLED];
        emb_leds(lv);
        CHECK(lv[B_COUNT + emb_key_of_white[1]] == LED_LIT, "the modifier's key lit while in effect");
        CHECK(lv[B_COUNT + emb_key_of_white[2]] == LED_OFF, "an idle modifier key dark");
    }
    emb_post(C_PANIC, 0, 0, 0, 0);
    emb_post(C_MOD, 1, 0, 0, 0);
    run(EMB_CTL * 2u, 0);
    ui.mode = 0;
}

static void test_volume_card(void)
{
    int32_t peak = 0;
    uint32_t i;
    run(EMB_CTL * 2u, 0);
    emb_master(128u << 4);
    emb_master(129u << 4);                          /* one step: the pot's jitter */
    CHECK(ui.vol_t == 0u || (int32_t)(ui.now - ui.vol_t) > 0, "jitter shows no card");
    emb_master(140u << 4);
    CHECK(ui.vol_t && (int32_t)(ui.now - ui.vol_t) < 0, "a turn shows the card");
    run(EMB_FS * 12u / 10u, 0);
    CHECK((int32_t)(ui.now - ui.vol_t) > 0, "gone a second later");
    emb_master(4095);                               /* full MASTER on the loudest scene: never past the ceiling */
    emb_post(C_QUEUE, 12, 0, 0, 0);
    for (i = 0; i < 300u; i++)
        run(1024, &peak);
    CHECK(peak <= OUT_CEIL[0], "limiter: peak %d over the ceiling %d", peak, OUT_CEIL[0]);
    CHECK(peak > OUT_CEIL[0] * 9 / 10, "and loud: peak %d", peak);
    emb_post(C_PANIC, 0, 0, 0, 0);
    emb_master(200u << 4);
}

static int shots(const char *dir)
{
    static uint16_t fb[EMB_W * EMB_H];
    uint32_t dirty[8], t;
    static const uint32_t BOOT_T[4] = {500, 1400, 1800, 3300};
    static const char *const BOOT_N[4] = {"boot1_crest", "boot2_bloom", "boot3_rose", "boot4_proclamation"};
    emb_power(76, 0);
    for (t = 0; t < 4u; t++) {
        emb_power(76, t == 3u ? 1 : 0);
        ui.booting = 1;
        ui.boot_started = 1;
        ui.boot_t0 = 1;
        ui.now = 1u + BOOT_T[t];
        emb_boot_note(0);
        emb_draw(fb, dirty);
        ppm(dir, BOOT_N[t], fb);
    }
    emb_boot_note(0);
    ui.booting = 0;
    run(EMB_FS / 2u, 0);
    emb_post(C_PLAY, 255, 0, 0, 0);
    emb_scene_factory(0, &emb_bench);               /* a workbench with something on it */
    run(EMB_FS, 0);
    ui.mode = 0;
    ui.page = P_HOME; emb_draw(fb, dirty); ppm(dir, "bardic_home", fb);
    ui.page = P_SEQ; ui.trk = 2; emb_draw(fb, dirty); ppm(dir, "bardic_steps", fb);
    ui.page = P_FX; emb_draw(fb, dirty); ppm(dir, "bardic_sanctuary", fb);
    ui.page = P_MODE; emb_draw(fb, dirty); ppm(dir, "bardic_modes", fb);
    ui.sub = 1; emb_draw(fb, dirty); ppm(dir, "bardic_box", fb);
    ui.sub = 0;
    ui.page = P_HOME;
    ui.vol_t = ui.now + 500u;
    emb_draw(fb, dirty); ppm(dir, "volume_card", fb);
    ui.vol_t = 0;
    ui.mode = 1;
    emb_post(C_QUEUE, 12, 0, 0, 0);
    run(EMB_FS * 3u, 0);
    emb_post(C_QUEUE, 14, 0, 0, 0);
    run(EMB_CTL * 8u, 0);
    ui.msg[0] = 0;
    emb_draw(fb, dirty);
    ppm(dir, "encounter", fb);
    printf("screenshots in %s\n", dir);
    return 0;
}

int main(int argc, char **argv)
{
    emb_init(1);
    if (argc > 4 && !strcmp(argv[1], "wav"))
        return wav((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), argv[4]);
    if (argc > 2 && !strcmp(argv[1], "shots"))
        return shots(argv[2]);
    test_math();
    test_scenes();
    test_sysex();
    test_play();
    test_crossfade();
    test_microtiming();
    test_purge_undo();
    test_ui();
    test_encounter_keys();
    test_volume_card();
    test_boot();
    printf(fails ? "%d CHECKS FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
