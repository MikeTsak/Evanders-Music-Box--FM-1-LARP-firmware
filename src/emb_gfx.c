/* SPDX-License-Identifier: GPL-3.0-only
 * Evander's Music Box: drawing. A 240x240 RGB565 frame (native byte order; the FM-1 port swaps for the
 * panel), primitives, a 5x7 font, the Mother Melody crest and the Blue Rose, the boot animation. */

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COL_NAVY RGB(0x0A, 0x11, 0x28)              /* deep mystical navy */
#define COL_NAVY2 RGB(0x16, 0x22, 0x48)             /* a panel on the navy */
#define COL_CYAN RGB(0x00, 0xE5, 0xFF)              /* luminous ice-cyan */
#define COL_CYAN_D RGB(0x00, 0x5A, 0x70)
#define COL_PARCH RGB(0xE8, 0xD9, 0xB0)             /* parchment */
#define COL_WOOD RGB(0x6B, 0x44, 0x23)              /* the frame's wood */
#define COL_ROSE RGB(0x1E, 0x4F, 0xD8)              /* the Blue Rose */
#define COL_ROSE_D RGB(0x0D, 0x2A, 0x80)
#define COL_WHITE RGB(0xF2, 0xF2, 0xF2)
#define COL_INK RGB(0x00, 0x00, 0x00)
#define COL_RED RGB(0xC0, 0x39, 0x2B)
#define COL_GOLD RGB(0xD4, 0xAF, 0x37)
#define COL_DIM RGB(0x5A, 0x6A, 0x90)
#define COL_EMBER RGB(0xFF, 0x7A, 0x2E)

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
static void text_glow(int cx, int y, const char *s, int sc, uint16_t c, uint16_t glow)   /* a halo, then the ink */
{
    int x = cx - text_w(s, sc) / 2;
    text(x - 1, y, s, sc, glow);
    text(x + 1, y, s, sc, glow);
    text(x, y - 1, s, sc, glow);
    text(x, y + 1, s, sc, glow);
    text(x, y, s, sc, c);
}

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

/* ------------------------------------------------------------------ the crest and the rose */
/* The Mother Melody crest: two sharp gothic M's sharing their middle stroke, a crown above, wings
 * flaring from the outer legs. p: how much is drawn, 0..256 (the stroke reveal); s: scale /256 */
static void crest(int cx, int cy, int p, int s, uint16_t c)
{
    static const int8_t M[][2] = {                  /* the double M as one stroke, units of 1/32 of its height */
        {-52, 20}, {-52, -16}, {-40, -30}, {-26, -2}, {-14, -30}, {0, -12},
        {14, -30}, {26, -2}, {40, -30}, {52, -16}, {52, 20}};
    static const int8_t CROWN[][2] = {{-18, -40}, {-12, -54}, {-6, -42}, {0, -60}, {6, -42}, {12, -54}, {18, -40}};
    int n = (int)(sizeof M / sizeof M[0]), seg, k, total = n - 1;
    int shown = (p * total) >> 8;
    for (seg = 0; seg < total && seg <= shown; seg++) {
        int x0 = cx + ((M[seg][0] * s) >> 8), y0 = cy + ((M[seg][1] * s) >> 8);
        int x1 = cx + ((M[seg + 1][0] * s) >> 8), y1 = cy + ((M[seg + 1][1] * s) >> 8);
        if (seg == shown) {                         /* the stroke being drawn */
            int f = (p * total) & 255;
            x1 = x0 + (((x1 - x0) * f) >> 8);
            y1 = y0 + (((y1 - y0) * f) >> 8);
        }
        line(x0, y0, x1, y1, (3 * s) >> 8, c);
    }
    if (p < 128)
        return;
    for (k = 0; k + 1 < 7; k++)                     /* the crown */
        line(cx + ((CROWN[k][0] * s) >> 8), cy + ((CROWN[k][1] * s) >> 8), cx + ((CROWN[k + 1][0] * s) >> 8),
             cy + ((CROWN[k + 1][1] * s) >> 8), (2 * s) >> 8, COL_GOLD);
    disc(cx, cy + ((-62 * s) >> 8), (3 * s) >> 8, COL_GOLD);
    for (k = 0; k < 4; k++) {                       /* the wings: feathers flaring out and up, longer outward */
        int len = ((22 + k * 9) * s) >> 8, side, a = 40 + k * 26;
        for (side = -1; side <= 1; side += 2) {
            int x0 = cx + side * ((54 * s) >> 8), y0 = cy + (((-10 + k * 8) * s) >> 8);
            line(x0, y0, x0 + side * ((len * emb_sine[(uint32_t)(256 - a) & 1023u]) >> 15),
                 y0 - ((len * emb_sine[(uint32_t)a & 1023u]) >> 15), (s >> 8) + 1, c);
        }
    }
}

/* the Blue Rose (the heraldic five-petal rose of assets/bluerose.png): s scale /256, rot in 1024ths */
static void rose(int cx, int cy, int s, int rot)
{
    int k;
    if (s <= 0)
        return;
    for (k = 0; k < 5; k++) {                       /* white sepal points between the petals, outlined */
        int a = rot + 102 + k * 205, b0 = a - 80, b1 = a + 80;
        int tx = cx + pol_x(a, (96 * s) >> 8), ty = cy + pol_y(a, (96 * s) >> 8);
        tri(tx, ty, cx + pol_x(b0, (40 * s) >> 8), cy + pol_y(b0, (40 * s) >> 8), cx + pol_x(b1, (40 * s) >> 8),
            cy + pol_y(b1, (40 * s) >> 8), COL_INK);
        tx = cx + pol_x(a, (88 * s) >> 8);
        ty = cy + pol_y(a, (88 * s) >> 8);
        tri(tx, ty, cx + pol_x(b0 + 10, (42 * s) >> 8), cy + pol_y(b0 + 10, (42 * s) >> 8),
            cx + pol_x(b1 - 10, (42 * s) >> 8), cy + pol_y(b1 - 10, (42 * s) >> 8), COL_WHITE);
    }
    for (k = 0; k < 5; k++)                         /* the petals' black outlines */
        disc(cx + pol_x(rot + k * 205, (34 * s) >> 8), cy + pol_y(rot + k * 205, (34 * s) >> 8), (34 * s) >> 8, COL_INK);
    for (k = 0; k < 5; k++) {
        int px0 = cx + pol_x(rot + k * 205, (34 * s) >> 8), py0 = cy + pol_y(rot + k * 205, (34 * s) >> 8);
        disc(px0, py0, (30 * s) >> 8, COL_ROSE);
        disc(px0 + pol_x(rot + k * 205, (8 * s) >> 8), py0 + pol_y(rot + k * 205, (8 * s) >> 8), (14 * s) >> 8,
             COL_ROSE_D);                           /* the inner fold */
        disc(px0 + pol_x(rot + k * 205, (11 * s) >> 8), py0 + pol_y(rot + k * 205, (11 * s) >> 8), (12 * s) >> 8,
             COL_ROSE);
    }
    for (k = 0; k < 5; k++)                         /* the black seams between the petals */
        line(cx, cy, cx + pol_x(rot + 102 + k * 205, (50 * s) >> 8), cy + pol_y(rot + 102 + k * 205, (50 * s) >> 8),
             (s >> 8) + 1, COL_INK);
    disc(cx, cy, (22 * s) >> 8, COL_INK);
    disc(cx, cy, (19 * s) >> 8, COL_WHITE);
    for (k = 0; k < 14; k++)                        /* the ring of red beads */
        disc(cx + pol_x(k * 73, (14 * s) >> 8), cy + pol_y(k * 73, (14 * s) >> 8), (s * 3) >> 9, COL_RED);
}

/* the boot animation at t ms: the crest is written, it yields to the rose blooming, the proclamation */
static void boot_frame(uint32_t t)
{
    static const char *const L[3] = {"EVANDER'S MUSIC BOX", "MOTHER MELODY", "HEARS THY SONG"};
    int i;
    fill(0, 0, EMB_W, EMB_H, COL_NAVY);
    for (i = 0; i < 40; i++) {                      /* a few fixed stars */
        uint32_t h = (uint32_t)i * 2654435761u;
        int x = (int)((h >> 8) & 255u), y = (int)((h >> 16) & 255u);
        if (x < EMB_W && y < EMB_H && ((t >> 7) + (uint32_t)i) % 7u)
            px(x, y, (h & 1u) ? COL_DIM : COL_CYAN_D);
    }
    frame(3, 3, EMB_W - 6, EMB_H - 6, COL_WOOD);
    frame(5, 5, EMB_W - 10, EMB_H - 10, COL_PARCH);
    if (t < 1100u) {                                /* the crest, written stroke by stroke */
        int p = (int)emb_udiv(t * 256u, 900u);
        crest(120, 94, p > 256 ? 256 : p, 300, COL_CYAN);
    } else if (t < 1900u) {                         /* the crest gives way to the rose */
        int f = (int)emb_udiv((t - 1100u) * 256u, 800u);
        if (f < 160)
            crest(120, 94 - (f >> 3), 256, 300 - f, f < 80 ? COL_CYAN : COL_CYAN_D);
        rose(120, 88, (f * 200) >> 8, 512 - (f * 2));
    } else {
        int k;
        static const uint8_t Y[3] = {176, 199, 216};
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
            text_glow(120, Y[k], b, 2, k ? COL_PARCH : COL_CYAN, COL_NAVY2);
            if (k == 0 && n > 19u) {                /* the em dash: a runic bar with a diamond */
                fill(40, 194, 160, 1, COL_CYAN_D);
                tri(120, 191, 116, 194, 120, 197, COL_CYAN);
                tri(120, 191, 124, 194, 120, 197, COL_CYAN);
            }
        }
    }
}
