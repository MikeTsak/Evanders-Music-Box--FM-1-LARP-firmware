/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box on the M-VAVE FM-1 (JieLi AC791N / WL82, pi32v2). One compilation unit, as Felucca builds:
 *
 *   upstream/felucca/firmware/hal/   the FM-1 hardware layer (registers, I2S DMA, key matrix, LCD SPI, flash,
 *                                    timers, IRQs, guards): Felucca 1.6.1, GPL-3.0-only, Hügelton Instruments
 *   upstream/felucca/firmware/src/   libc.c, lcd.c, usb.c (class-compliant USB-MIDI), ota.c (M-UPGRADE updates)
 *   src/                             the Music Box core (emb.c)
 *
 * Interrupts (as Felucca): ALNK0 (I2S half buffer, prio 3) renders the core; TIMER5 (10 kHz, prio 4) scans the
 * keys and LEDs, counts ms and polls USB at 2 kHz (never nested in the render: the scan only). Everything else
 * is the main loop: input -> core, core -> LEDs, the screen, MIDI in, the update protocol, the SysEx console.
 *
 * Kept from the stock firmware, so an FM-1 can always be updated or rescued:
 *   - the M-UPGRADE protocol (ota.c): the web installer, tools/fm1_install.py and M-VAVE's updater all work
 *   - SysEx F0 22 24 35 7D F7 and OCT- + OCT+ held 5 s: the chip's UBOOT (mask ROM) for jl-uboot-tool
 *   - two failed boots in a row (a crash in the first 30 s): UBOOT on its own */
#define FELUCCA_FLASH 1
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FM1_CPU1_USED 0                              /* one core: the second stays halted */
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_777"                        /* the package identity (tools/build.py --product) */
#endif
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define EMB_POOL __attribute__((section(".pool")))
#define EMB_BARRIER() __asm__ volatile("" ::: "memory")

#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "libc.c"
#include "lcd.c"
#include "usb.c"
#include "../emb.c"                                  /* the core */

/* ------------------------------------------------------------------ boot state that survives resets */
#define BOOTGUARD_MAGIC 0x42475244u
static struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
static uint8_t flash_ok;

/* ------------------------------------------------------------------ audio: ALNK0 I2S, double-buffered DMA */
#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint8_t in_audio;
static volatile uint32_t audio_us_max, audio_late;

static void audio_silence(void)                     /* (IRQs off around flash work: the DMA loops the buffer) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
}

void fm1_alnk0_irq(void)                            /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    in_audio = 1;                                   /* first: TIMER5 nests from here on */
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), us;
        emb_render(&abuf[half * HALF_WORDS], HALF_FRAMES);   /* interleaved L R, Q15: what the codec takes */
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        if (us > audio_us_max)
            audio_us_max = us;
        if (fm1_audio_free_half() != half)
            audio_late++;                           /* the DMA moved on while we rendered */
        if (us > 2300u)                             /* over ~80 % of the 2.9 ms: lighten the next one */
            emb_shed();
        emb_st.cpu = (uint8_t)(us > 2902u ? 255u : us * 255u / 2902u);   /* of a half buffer's 2.9 ms */
    }
    in_audio = 0;
}
extern void isr_alnk0(void);

/* ------------------------------------------------------------------ TIMER5: scan, ms, USB */
void fm1_timer5_irq(void)
{
    static uint32_t sub, acc, last;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (++sub == 10u)
        sub = 0;
    if (!in_audio && usb_due)                       /* 2 kHz; a late poll catches up on the next tick */
        usb_poll();
}
extern void isr_timer5(void);

/* ------------------------------------------------------------------ the update protocol (ota.c) and its hooks */
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_erase(uint32_t off)
{
    uint32_t took, f;
    int rc;
    if (!FL_IN(off, 0x1000u, OTA_AREA, OTA_AREA + OTA_AREA_LEN) || (off & 0xFFFu))
        return -8;
    f = irq_save();
    audio_silence();
    rc = FL_FAR(fl_erase4k_ram)(off, &took);
    irq_restore(f);
    return rc;
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN))
        return -8;
    return fl_write(off, p, n);
}
static int st_read(uint32_t off, void *dst, uint32_t n)   /* 256-byte IRQ-off windows: the audio keeps up */
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    (void)code;
    lcd_fill(0, 0, 240, 240, COL_NAVY);
    lcd_fill(20, 110, step < 9u ? step * 40u : 200u, 20, COL_CYAN);   /* a cyan bar: the update's steps */
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;                          /* an intentional reset */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

/* ------------------------------------------------------------------ persistence: one 4 KiB sector per slot from Felucca's data area (0x97000: settings, workbench, 16 scenes,
 * 0x97000 .. 0xA9000). A record is only believed with its magic and checksum (emb_ui.c), so whatever another
 * firmware left there reads as "empty" and the factory scenes load. */
#define EMB_FL_BASE FL_DATA_LO
static uint8_t st_buf[0x1000] EMB_POOL;

int emb_hal_store(uint32_t slot, const void *p, uint32_t n)
{
    uint32_t off = EMB_FL_BASE + slot * 0x1000u, took, f;
    int rc;
    if (!flash_ok || n > 0x1000u || slot >= 2u + EMB_NSLOT || !FL_STORE_OK(off, 0x1000u))
        return -1;
    memcpy(st_buf, p, n);                           /* (the source must be RAM: it is, this copy is the record) */
    f = irq_save();
    audio_silence();
    fm1__led_lines(0);
    rc = FL_FAR(fl_erase4k_ram)(off, &took);
    irq_restore(f);
    if (!rc)
        rc = fl_write(off, st_buf, n);
    return rc;
}
int emb_hal_load(uint32_t slot, void *p, uint32_t n)
{
    if (!flash_ok || n > 0x1000u || slot >= 2u + EMB_NSLOT)
        return -1;
    return st_read(EMB_FL_BASE + slot * 0x1000u, p, n);
}

/* ------------------------------------------------------------------ the panel: matrix ids <-> labels */
/* matrix button id per label (src/panel.c PANEL_DEFAULT, measured by Felucca), our order FX SEL ENV LFO EDIT GLO
 * HOME SAVE ARP SEQ PLAY REC OCT- OCT+; encoders SELECT PRESETS ALGORITHM K1..K4 */
static const uint8_t BTN_ID[B_COUNT] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1};
static const uint8_t ENC_ID[E_COUNT] = {0, 6, 1, 2, 3, 4, 5};
static uint8_t led_pos[EMB_NLED];                   /* column << 3 | row of each LED, 0xFF none */

static void leds_init(void)
{
    uint32_t i, c, r;
    for (i = 0; i < EMB_NLED; i++) {
        uint32_t id = i < B_COUNT ? BTN_ID[i] : FM1_ANIM_NBTN + (i - B_COUNT);
        led_pos[i] = 0xFFu;
        for (c = 0; c < FM1_NCOL; c++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][c] == (int8_t)id)
                    led_pos[i] = (uint8_t)(c << 3 | r);
    }
}

static void leds_write(void)
{
    uint8_t lv[EMB_NLED], lit[FM1_NCOL] = {0}, dim[FM1_NCOL] = {0}, mid[FM1_NCOL] = {0};
    uint32_t i;
    if (fm1_led_anim_on())
        return;                                     /* the power-on sweep still runs */
    emb_leds(lv);
    for (i = 0; i < EMB_NLED; i++) {
        uint32_t c = led_pos[i] >> 3, m = 1u << (led_pos[i] & 7u);
        if (led_pos[i] == 0xFFu)
            continue;
        if (lv[i] == LED_LIT)
            lit[c] |= (uint8_t)m;
        else if (lv[i] == LED_MID) {
            mid[c] |= (uint8_t)m;
            dim[c] |= (uint8_t)m;
        } else if (lv[i] == LED_GLOW)
            dim[c] |= (uint8_t)m;
    }
    for (i = 0; i < FM1_NCOL; i++) {                /* (bytes: the scan reads each one whole) */
        fm1_led[i] = lit[i];
        fm1_led_dim[i] = dim[i];
        fm1_led_mid[i] = mid[i];
    }
}

static void input_poll(void)
{
    static uint32_t notes_prev;
    uint32_t rel, press = fm1_input_edges(&rel), notes = fm1_in.notes, ch = notes ^ notes_prev, i;
    for (i = 0; i < B_COUNT; i++) {
        if ((press >> BTN_ID[i]) & 1u)
            emb_button(i, 1);
        if ((rel >> BTN_ID[i]) & 1u)
            emb_button(i, 0);
    }
    for (i = 0; ch && i < EMB_NKEY; i++)
        if ((ch >> i) & 1u)
            emb_key(i, (int)((notes >> i) & 1u), 100);   /* the FM-1's keys report no velocity */
    notes_prev = notes;
    for (i = 0; i < E_COUNT; i++) {
        int32_t s = fm1_enc_take(ENC_ID[i]);
        if (s)
            emb_encoder(i, s);
    }
}

static void master_poll(void)                       /* the MASTER pot (ADC ch 4, 10 bit), smoothed */
{
    static int32_t v = -1;
    int32_t r = fm1_adc_read(FM1_ADC_MASTER);
    if (r < 0)
        return;
    v = v < 0 ? r << 4 : v + ((r << 4) - v) / 8;
    emb_master((uint32_t)v >> 2);
}

static void midi_poll(void)                         /* USB-MIDI in: notes, program changes, clock start / stop */
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        RING_PUBLISH();
        mi_r++;
        emb_midi((uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16), (uint8_t)(pkt >> 24));
    }
    if (midi_in_overflow) {
        midi_in_overflow = 0;
        mi_r = mi_w;
    }
}

static void sysex_poll(void)                        /* the preset console (emb_sysex) shares the frame with ota.c */
{
    static uint8_t in[640], out[700], wire[820];
    const uint8_t *p;
    uint32_t n, k, w;
    if (!ota_frame_get(&p, &n) || n < 3u || p[0] != 0x7Du || p[1] != 0x45u || p[2] != 0x4Du)
        return;
    for (k = 0; k < n && k < sizeof in; k++)
        in[k] = p[k];
    ota_frame_done();
    n = emb_sysex(in, k, out);
    if (!n)
        return;
    wire[0] = 0xF0;
    for (w = 0; w < n && w + 2u < sizeof wire; w++)
        wire[w + 1u] = out[w];
    wire[w + 1u] = 0xF7;
    ota_wire_send(wire, w + 2u);
}

/* ------------------------------------------------------------------ the screen */
static uint16_t fb[EMB_W * EMB_H] EMB_POOL;
static uint16_t strip[16u * EMB_W] EMB_POOL;

static void screen_frame(void)
{
    uint32_t dirty[(EMB_H + 31) / 32], y = 0;
    if (!emb_draw(fb, dirty))
        return;
    while (y < EMB_H) {
        uint32_t y0, n, i;
        if (!((dirty[y >> 5] >> (y & 31u)) & 1u)) {
            y++;
            continue;
        }
        y0 = y;
        while (y < EMB_H && y - y0 < 16u && ((dirty[y >> 5] >> (y & 31u)) & 1u))
            y++;
        n = (y - y0) * EMB_W;
        lcd_sync();                                 /* the last strip has left before this one is written */
        for (i = 0; i < n; i++) {                   /* the panel takes RGB565 big-endian */
            uint16_t c = fb[y0 * EMB_W + i];
            strip[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        lcd_blit(0, y0, EMB_W, y - y0, strip);
    }
}

/* ------------------------------------------------------------------ faults: a screen, then a reset */
static void fm1_fault(const fm1_crash_t *c)
{
    uint32_t t0;
    (void)c;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, COL_NAVY);
    lcd_fill(0, 100, 240, 40, COL_RED);             /* a red band: the box stumbled, it restarts */
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 3000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void uboot_now(void)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, COL_NAVY);
    lcd_fill(60, 110, 120, 20, COL_CYAN);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                          /* intentional: not a failed boot */
    fm1_enter_uboot();
}

static void fm1_main(void)
{
    uint32_t f = irq_save(), last_frame = 0, oct_t0 = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u; /* the FM-1's 1 MiB part; anything else: RAM only */
    irq_restore(f);
    if (flash_ok)
        ota_boot_cleanup();                         /* the staging area an update left */
    lcd_init();
    fm1_input_init();
    fm1_adc_init();
    leds_init();
    emb_init(fm1_ticks());
    master_poll();
    audio_silence();
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
    fm1_led_dim_level(0);
    fm1_led_anim_start(1);                          /* Felucca's power-on light sweep under the boot animation */
    fm1_timer5_start(isr_timer5, 4);                /* above ALNK0: the scan nests into the render */
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    for (;;) {
        uint32_t now = fm1_ms;
        fm1_wdt_feed();
        usb_retry(now);
        if (now > 30000u && bootguard.pending) {   /* a crash or hang in the first 30 s counts as a failed boot */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {   /* OCT- + OCT+ held 5 s: the chip's UBOOT (jl-uboot-tool, unbrickers); letting go cancels */
            uint32_t both = (1u << BTN_ID[B_OCTDN]) | (1u << BTN_ID[B_OCTUP]);
            if ((fm1_in.buttons & both) != both)
                oct_t0 = now;
            else if (now - oct_t0 > 5000u)
                uboot_now();
            else if (now - oct_t0 > 2000u && ((now - oct_t0) % 1000u) < 20u)
                say("UBOOT: KEEP HOLDING");
        }
        if (usb.uboot_req)                          /* SysEx F0 22 24 35 7D F7 */
            uboot_now();
        sysex_poll();
        ota_service();                              /* the M-UPGRADE handshake: our identity */
        if (usb.ota_req) {                          /* F0 22 24 35 7F F7: an update */
            usb.ota_req = 0;
            emb_post(C_PANIC, 0, 0, 0, 0);
            fm1_delay_ms(20);
            if (flash_ok)
                ota_session();                      /* returns only if nothing was committed */
        }
        midi_poll();
        input_poll();
        master_poll();
        emb_tick(now);
        if (now - last_frame >= 33u) {              /* ~30 frames a second: LEDs and the changed rows */
            last_frame = now;
            leds_write();
            screen_frame();
        }
    }
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);                              /* ~8 s, fed by the main loop */
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {                   /* two boots died young: the chip's own update mode */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                    /* the flash driver: it runs from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
