/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: drawing. A 240x240 RGB565 frame (native byte order; the FM-1 port swaps for the panel).
 * The look: flat blocks of bold colour on ink, hard offset shadows, a 5x7 pixel font, little 1-bit sprites
 * (a pocket synth's screen crossed with a party game). Primitives, sprites, the crest, the Blue Rose, the boot. */

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COL_INK RGB(0x10, 0x10, 0x10)               /* the ground */
#define COL_INK2 RGB(0x26, 0x26, 0x26)              /* a panel on it */
#define COL_GREY RGB(0x6E, 0x6E, 0x6E)
#define COL_PAPER RGB(0xF2, 0xEE, 0xE4)             /* text, cards */
#define COL_BLUE RGB(0x2E, 0x5B, 0xFF)              /* the Blue Rose; K1; Encounter */
#define COL_GREEN RGB(0x19, 0xC3, 0x7D)             /* K2; REST */
#define COL_LIGHT RGB(0xDC, 0xDC, 0xD8)             /* K3 */
#define COL_ORANGE RGB(0xFF, 0x5A, 0x1F)            /* K4; Bardic; COMBAT */
#define COL_YELLOW RGB(0xFF, 0xD2, 0x1F)            /* EXPLORE */
#define COL_PINK RGB(0xFF, 0x4F, 0x9A)              /* CLIMAX; warnings */
#define COL_PURPLE RGB(0x7B, 0x4D, 0xFF)
#define COL_BLUE_D RGB(0x16, 0x2E, 0x96)
#define COL_RED RGB(0xE8, 0x33, 0x3A)
/* (names the FM-1 port's update and crash screens use) */
#define COL_NAVY COL_INK
#define COL_CYAN COL_BLUE

static const uint16_t KNOB_COL[4] = {COL_BLUE, COL_GREEN, COL_LIGHT, COL_ORANGE};   /* K1..K4, as the knobs on a
                                                                                    * certain Swedish synth */
static const uint16_t TRACK_COL[4] = {COL_ORANGE, COL_PURPLE, COL_BLUE, COL_YELLOW};
static const uint16_t FAMILY_COL[4] = {COL_GREEN, COL_YELLOW, COL_ORANGE, COL_PINK};

static uint16_t *G;                                 /* the frame being drawn */

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EMB_W) w = EMB_W - x;
    if (y + h > EMB_H) h = EMB_H - y;
    for (j = 0; j < h; j++) {
        uint16_t *p = G + (y + j) * EMB_W + x;
        for (i = 0; i < w; i++)
            p[i] = c;
    }
}
static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < EMB_W && (unsigned)y < EMB_H)
        G[y * EMB_W + x] = c;
}
static void frame(int x, int y, int w, int h, uint16_t c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}
static void disc(int cx, int cy, int r, uint16_t c)
{
    int y, x = r;
    for (y = 0; y <= r; y++) {
        while (x > 0 && x * x + y * y > r * r + r)
            x--;
        fill(cx - x, cy - y, 2 * x + 1, 1, c);
        if (y)
            fill(cx - x, cy + y, 2 * x + 1, 1, c);
    }
}
static int edge(int ax, int ay, int bx, int by, int x, int y) { return (bx - ax) * (y - ay) - (by - ay) * (x - ax); }
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    int minx = x0, maxx = x0, miny = y0, maxy = y0, x, y, area = edge(x0, y0, x1, y1, x2, y2);
    if (!area)
        return;
    if (x1 < minx) minx = x1;
    if (x2 < minx) minx = x2;
    if (x1 > maxx) maxx = x1;
    if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1;
    if (y2 < miny) miny = y2;
    if (y1 > maxy) maxy = y1;
    if (y2 > maxy) maxy = y2;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++) {
            int a = edge(x1, y1, x2, y2, x, y), b = edge(x2, y2, x0, y0, x, y), d = edge(x0, y0, x1, y1, x, y);
            if (area > 0 ? (a >= 0 && b >= 0 && d >= 0) : (a <= 0 && b <= 0 && d <= 0))
                px(x, y, c);
        }
}
static void line(int x0, int y0, int x1, int y1, int r, uint16_t c)   /* a thick line: discs along it */
{
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy), i;
    if (!n)
        n = 1;
    for (i = 0; i <= n; i++) {
        int x = x0 + (int)emb_udiv((uint32_t)((dx < 0 ? -dx : dx) * i), (uint32_t)n) * (dx < 0 ? -1 : 1);
        int y = y0 + (int)emb_udiv((uint32_t)((dy < 0 ? -dy : dy) * i), (uint32_t)n) * (dy < 0 ? -1 : 1);
        if (r)
            disc(x, y, r, c);
        else
            px(x, y, c);
    }
}
/* polar: angle a in 1024ths of a turn (0 = up, clockwise), radius r -> offset */
static int pol_x(int a, int r) { return (r * emb_sine[(uint32_t)a & 1023u]) >> 15; }
static int pol_y(int a, int r) { return -((r * emb_sine[(uint32_t)(a + 256) & 1023u]) >> 15); }

/* ---- 5x7 font, ASCII 32..95, columns bottom-up bits (lowercase prints as uppercase) ---- */
static const uint8_t FONT[64][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}};

static int text(int x, int y, const char *s, int sc, uint16_t c)   /* returns the end x */
{
    for (; *s; s++) {
        int ch = *s, col, row;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (ch < 32 || ch > 95)
            ch = '?';
        for (col = 0; col < 5; col++) {
            uint32_t bits = FONT[ch - 32][col];
            for (row = 0; row < 7; row++)
                if ((bits >> row) & 1u)
                    fill(x + col * sc, y + row * sc, sc, sc, c);
        }
        x += 6 * sc;
    }
    return x;
}
static int text_w(const char *s, int sc)
{
    int n = 0;
    while (s[n])
        n++;
    return n ? n * 6 * sc - sc : 0;
}
static void text_c(int cx, int y, const char *s, int sc, uint16_t c) { text(cx - text_w(s, sc) / 2, y, s, sc, c); }

static void num(char *b, int32_t v)                 /* a short decimal */
{
    char t[12];
    int n = 0, i = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do {
        uint32_t q = emb_udiv(u, 10u);
        t[n++] = (char)('0' + (u - q * 10u));
        u = q;
    } while (u && n < 11);
    if (v < 0)
        b[i++] = '-';
    while (n)
        b[i++] = t[--n];
    b[i] = 0;
}
static void cat(char *d, const char *s)
{
    while (*d)
        d++;
    while ((*d++ = *s++))
        ;
}

static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_name(char *b, int n)
{
    uint32_t u = (uint32_t)(n + 120), o = u / 12u, pc = u % 12u;   /* (constants) */
    b[0] = 0;
    cat(b, NOTE_NAME[pc]);
    {
        char t[4];
        num(t, (int32_t)o - 11);
        cat(b, t);
    }
}

static void text_sh(int x, int y, const char *s, int sc, uint16_t c, uint16_t sh)   /* a hard drop shadow */
{
    text(x + sc, y + sc, s, sc, sh);
    text(x, y, s, sc, c);
}
static void text_csh(int cx, int y, const char *s, int sc, uint16_t c, uint16_t sh)
{
    text_sh(cx - text_w(s, sc) / 2, y, s, sc, c, sh);
}
static void card(int x, int y, int w, int h, uint16_t c, uint16_t edge_c)   /* a flat card, a hard shadow */
{
    fill(x + 3, y + 3, w, h, COL_INK);
    fill(x, y, w, h, edge_c);
    fill(x + 2, y + 2, w - 4, h - 4, c);
}

/* ------------------------------------------------------------------ sprites: 1-bit, 9 wide, row bit 8 = left */
enum { SP_FIRE, SP_TREE, SP_SWORDS, SP_CROWN, SP_ROSE, SP_NOTE, SP_SPEAKER, SP_BOLT, SP_DRUM, SP_COUNT };
static const uint16_t SPRITE[SP_COUNT][9] = {
    {0x010, 0x018, 0x038, 0x078, 0x07C, 0x0EE, 0x0C6, 0x1FF, 0x0AA},   /* a campfire: REST */
    {0x010, 0x038, 0x07C, 0x038, 0x07C, 0x0FE, 0x1FF, 0x010, 0x038},   /* a tree: EXPLORE */
    {0x101, 0x082, 0x044, 0x028, 0x010, 0x028, 0x0C6, 0x082, 0x101},   /* crossed swords: COMBAT */
    {0x000, 0x111, 0x1BB, 0x1FF, 0x1FF, 0x155, 0x1FF, 0x000, 0x000},   /* a crown: CLIMAX */
    {0x028, 0x07C, 0x0EE, 0x0D6, 0x0EE, 0x07C, 0x038, 0x010, 0x008},   /* the Blue Rose */
    {0x01C, 0x01E, 0x013, 0x010, 0x010, 0x070, 0x0F0, 0x0F0, 0x060},   /* a note */
    {0x008, 0x018, 0x03A, 0x1F9, 0x1F9, 0x1F9, 0x03A, 0x018, 0x008},   /* a speaker */
    {0x00C, 0x018, 0x030, 0x07C, 0x018, 0x030, 0x060, 0x040, 0x000},   /* a bolt: USB power */
    {0x07C, 0x0FE, 0x1FF, 0x1FF, 0x183, 0x1FF, 0x1FF, 0x0FE, 0x07C},   /* a drum */
};
static void sprite(int x, int y, uint32_t id, int sc, uint16_t c)
{
    int r, k;
    for (r = 0; r < 9; r++)
        for (k = 0; k < 9; k++)
            if ((SPRITE[id][r] >> (8 - k)) & 1u)
                fill(x + k * sc, y + r * sc, sc, sc, c);
}

/* the battery, 22 x 11 at x, y: pct 0..100 (255 unknown), ext: on USB (a bolt over it) */
static void battery(int x, int y, uint32_t pct, uint32_t ext)
{
    int bars = pct > 100u ? 4 : (int)((pct + 12u) / 25u);
    uint16_t c = pct <= 15u ? COL_PINK : COL_PAPER, k;
    fill(x, y, 20, 11, c);
    fill(x + 20, y + 3, 2, 5, c);
    fill(x + 1, y + 1, 18, 9, COL_INK);
    for (k = 0; k < 4 && (int)k < bars; k++)
        fill(x + 2 + k * 4, y + 2, 3, 7, ext ? COL_GREEN : c);
    if (ext)
        sprite(x + 6, y + 1, SP_BOLT, 1, COL_YELLOW);
}

/* ------------------------------------------------------------------ the crest and the rose */
/* The Mother Melody logo: elegant curved MM monogram with central divider, flowing musical staff
 * waves and dancing notes in white and blue (assets/bluerose.png palette). No wings or crown.
 * p: stroke reveal 0..256; s: scale /256 */
static void crest(int cx, int cy, int p, int s, uint16_t c)
{
    /* Left half of the curved MM monogram; mirrored across x = cx */
    static const int8_t M_LEFT[][2] = {
        {-80, 6}, {-72, 12}, {-60, 8}, {-50, -4}, {-42, -20}, {-38, -36},
        {-34, -22}, {-28, -4}, {-22, 12}, {-16, 24}, {-20, 28}, {-25, 23},
        {-22, 10}, {-16, -6}, {-10, -22}, {-5, -34}, {-3, -16}, {-2, 4}, {-2, 22}
    };
    /* Flowing musical staff lines: waving outward behind the monogram */
    static const int8_t STAFF[][2] = {
        {-10, 0}, {-24, -4}, {-40, -5}, {-58, 2}, {-72, 3}, {-86, -6}, {-94, -14}
    };
    /* Musical notes along the staves: {dx, dy, stem_h, type: 0=single, 1=quaver} */
    static const int8_t NOTES[][4] = {
        {-78, -3, -8, 1}, {-62, 2, -9, 0}, {-46, -4, -9, 1}, {-32, -3, -8, 0},
        {32, -3, -8, 0}, {46, -4, -9, 1}, {62, 2, -9, 0}, {78, -3, -8, 1}
    };
    int n = (int)(sizeof M_LEFT / sizeof M_LEFT[0]), seg, k, side, total = n - 1;
    int shown = (p * total) >> 8;
    int staff_n = (int)(sizeof STAFF / sizeof STAFF[0]) - 1;
    int staff_shown = (p * staff_n) >> 8;
    int r_line = (3 * s) >> 8;
    if (r_line < 1) r_line = 1;

    /* 1. Behind the M: flowing musical staff waves */
    for (side = -1; side <= 1; side += 2) {
        int st;
        for (st = -6; st <= 6; st += 4) {
            int y_off = (st * s) >> 8;
            for (seg = 0; seg < staff_n && seg <= staff_shown; seg++) {
                int x0 = cx + side * ((STAFF[seg][0] * s) >> 8);
                int y0 = cy + y_off + ((STAFF[seg][1] * s) >> 8);
                int x1 = cx + side * ((STAFF[seg + 1][0] * s) >> 8);
                int y1 = cy + y_off + ((STAFF[seg + 1][1] * s) >> 8);
                if (seg == staff_shown) {
                    int f = (p * staff_n) & 255;
                    x1 = x0 + (((x1 - x0) * f) >> 8);
                    y1 = y0 + (((y1 - y0) * f) >> 8);
                }
                /* Staves in subtle blue depth */
                line(x0, y0, x1, y1, 0, p < 180 ? COL_BLUE_D : COL_BLUE);
            }
        }
    }

    /* 2. Musical notes placed along the staves */
    if (p >= 60) {
        int num_notes = (int)(sizeof NOTES / sizeof NOTES[0]);
        int notes_to_draw = ((p - 60) * num_notes) / 140;
        if (notes_to_draw > num_notes) notes_to_draw = num_notes;
        for (k = 0; k < notes_to_draw; k++) {
            int nx = cx + ((NOTES[k][0] * s) >> 8);
            int ny = cy + ((NOTES[k][1] * s) >> 8);
            int sh = (NOTES[k][2] * s) >> 8;
            disc(nx, ny, (2 * s) >> 8 > 0 ? (2 * s) >> 8 : 1, COL_PAPER);
            line(nx + 1, ny, nx + 1, ny + sh, 0, COL_PAPER);
            if (NOTES[k][3]) /* quaver flag */
                line(nx + 1, ny + sh, nx + 3, ny + sh + 3, 0, COL_BLUE);
        }
    }

    /* 3. Center vertical staff bar / divider */
    {
        int y0 = cy + ((-44 * s) >> 8), y1 = cy + ((30 * s) >> 8);
        int bar_prog = p > 120 ? 256 : (p * 256) / 120;
        int cur_y1 = y0 + (((y1 - y0) * bar_prog) >> 8);
        line(cx + 1, y0 + 1, cx + 1, cur_y1 + 1, 1, COL_BLUE_D);
        line(cx, y0, cx, cur_y1, 1, c);
    }

    /* 4. Symmetrical curved MM monogram (both left and right sides) */
    for (side = -1; side <= 1; side += 2) {
        for (seg = 0; seg < total && seg <= shown; seg++) {
            int x0 = cx + side * ((M_LEFT[seg][0] * s) >> 8);
            int y0 = cy + ((M_LEFT[seg][1] * s) >> 8);
            int x1 = cx + side * ((M_LEFT[seg + 1][0] * s) >> 8);
            int y1 = cy + ((M_LEFT[seg + 1][1] * s) >> 8);
            if (seg == shown) {
                int f = (p * total) & 255;
                x1 = x0 + (((x1 - x0) * f) >> 8);
                y1 = y0 + (((y1 - y0) * f) >> 8);
            }
            /* Deep blue under-stroke shadow, then crisp paper stroke */
            line(x0 + 1, y0 + 1, x1 + 1, y1 + 1, r_line, COL_BLUE_D);
            line(x0, y0, x1, y1, r_line, c);
        }
    }
}

/* the Blue Rose (the heraldic five-petal rose of assets/bluerose.png), flat: s scale /256, rot in 1024ths */
static void rose(int cx, int cy, int s, int rot)
{
    int k;
    if (s <= 0)
        return;
    for (k = 0; k < 5; k++) {                       /* the paper sepal points between the petals */
        int a = rot + 102 + k * 205, b0 = a - 80, b1 = a + 80;
        tri(cx + pol_x(a, (92 * s) >> 8), cy + pol_y(a, (92 * s) >> 8), cx + pol_x(b0, (40 * s) >> 8),
            cy + pol_y(b0, (40 * s) >> 8), cx + pol_x(b1, (40 * s) >> 8), cy + pol_y(b1, (40 * s) >> 8), COL_PAPER);
    }
    for (k = 0; k < 5; k++)                         /* a hard shadow under the petals */
        disc(cx + 4 + pol_x(rot + k * 205, (34 * s) >> 8), cy + 4 + pol_y(rot + k * 205, (34 * s) >> 8),
             (32 * s) >> 8, COL_BLUE_D);
    for (k = 0; k < 5; k++) {
        int px0 = cx + pol_x(rot + k * 205, (34 * s) >> 8), py0 = cy + pol_y(rot + k * 205, (34 * s) >> 8);
        disc(px0, py0, (31 * s) >> 8, COL_BLUE);
        disc(px0 + pol_x(rot + k * 205, (10 * s) >> 8), py0 + pol_y(rot + k * 205, (10 * s) >> 8), (11 * s) >> 8,
             COL_BLUE_D);                           /* the inner fold */
        disc(px0 + pol_x(rot + k * 205, (13 * s) >> 8), py0 + pol_y(rot + k * 205, (13 * s) >> 8), (9 * s) >> 8,
             COL_BLUE);
    }
    for (k = 0; k < 5; k++)                         /* the seams between the petals */
        line(cx, cy, cx + pol_x(rot + 102 + k * 205, (46 * s) >> 8), cy + pol_y(rot + 102 + k * 205, (46 * s) >> 8),
             (s >> 8) + 1, COL_INK);
    disc(cx, cy, (21 * s) >> 8, COL_INK);
    disc(cx, cy, (18 * s) >> 8, COL_PAPER);
    for (k = 0; k < 14; k++)                        /* the ring of red beads */
        disc(cx + pol_x(k * 73, (13 * s) >> 8), cy + pol_y(k * 73, (13 * s) >> 8), (s * 3) >> 9, COL_RED);
}

/* the boot animation at t ms: the Mother Melody logo is written, it yields to the Blue Rose blooming, the proclamation.
 * note: a line under it (the last reset's cause, from the FM-1 port), or 0 */
#define BOOT_MS 3600u
static void boot_frame(uint32_t t, const char *note)
{
    static const char *const L[3] = {"EVANDER'S MUSIC BOX", "MOTHER MELODY", "HEARS THY SONG"};
    /* White and blue starlight twinkling in the night sky (Blue Rose palette) */
    static const uint16_t CONF[5] = {COL_BLUE, COL_PAPER, COL_BLUE_D, COL_LIGHT, COL_BLUE};
    int i;
    fill(0, 0, EMB_W, EMB_H, COL_INK);
    for (i = 0; i < 28; i++) {                      /* pixel starlight, twinkling */
        uint32_t h = (uint32_t)i * 2654435761u;
        int x = (int)((h >> 8) & 255u), y = (int)((h >> 16) & 255u);
        if (x < EMB_W - 4 && y < 170 && ((t >> 7) + (uint32_t)i) % 5u)
            fill(x, y, 3, 3, CONF[h % 5u]);
    }
    if (t < 1100u) {                                /* the Mother Melody logo, written stroke by stroke */
        int p = (int)emb_udiv(t * 256u, 500u);
        crest(120, 94, p > 256 ? 256 : p, 256, COL_PAPER);
    } else if (t < 1900u) {                         /* the crest yields to the Blue Rose blooming */
        int f = (int)emb_udiv((t - 1100u) * 256u, 800u);
        if (f < 160)
            crest(120, 94 - (f >> 3), 256, 256 - f, f < 80 ? COL_PAPER : COL_BLUE_D);
        rose(120, 88, (f * 200) >> 8, 512 - (f * 2));
    } else {
        static const uint8_t Y[3] = {174, 196, 213};
        int k;
        rose(120, 88, 200, 0);
        for (k = 0; k < 3; k++) {                   /* the proclamation, a letter every 30 ms */
            uint32_t start = 1950u + (uint32_t)k * 420u, n;
            char b[24];
            if (t < start)
                break;
            n = emb_udiv(t - start, 30u);
            for (i = 0; L[k][i] && (uint32_t)i < n && i < 23; i++)
                b[i] = L[k][i];
            b[i] = 0;
            /* Proclamation in white and blue (paper letters with blue drop shadow) */
            text_csh(120, Y[k], b, 2, COL_PAPER, COL_BLUE_D);
        }
    }
    text(EMB_W - 4 - text_w("V" EMB_VERSION, 1), 4, "V" EMB_VERSION, 1, COL_GREY);   /* which firmware this is */
    if (note && note[0]) {                          /* why the last run ended, if it did not end well */
        fill(0, 230, EMB_W, 10, COL_PINK);
        text_c(120, 231, note, 1, COL_INK);
    }
}

