#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libdmview.h"
#include "../src/layout.h"
#include <errno.h>
#include <string.h>

/*
 * The views come from the .dmvs files in fixtures/, assembled by todmv before the test
 * runs (run_tests.sh); they are drawn into framebuffers in memory.
 */

#ifndef LIBDMVIEW_FIXTURES_DIR
#define LIBDMVIEW_FIXTURES_DIR "fixtures"
#endif
#define FIXTURE(name)   LIBDMVIEW_FIXTURES_DIR "/" name

#ifndef LIBDMVIEW_FONTS_DIR
#define LIBDMVIEW_FONTS_DIR "fonts"
#endif

#define W   64
#define H   48

static uint32_t g_fb32[W * H];
static uint16_t g_fb16[W * H];
static libdmview_surface_t g_s32, g_s16;
static libdmview_t g_view;

void dmod_test_setup(void)
{
    memset(g_fb32, 0x55, sizeof(g_fb32));
    memset(g_fb16, 0x55, sizeof(g_fb16));
    g_s32.pixels = g_fb32;
    g_s32.width = W;
    g_s32.height = H;
    g_s32.stride = W * 4;
    g_s32.format = DMDRVI_GFX_PIXEL_FORMAT_ARGB8888;
    g_s16.pixels = g_fb16;
    g_s16.width = W;
    g_s16.height = H;
    g_s16.stride = W * 2;
    g_s16.format = DMDRVI_GFX_PIXEL_FORMAT_RGB565;
    g_view = NULL;
    Dmod_SetEnv("DMVIEW_FONTS", "", 1);         /* The built-in font unless a step sets it */
}

void dmod_test_teardown(void)
{
    libdmview_close(g_view);
    g_view = NULL;
}

static uint32_t px(int x, int y) { return g_fb32[y * W + x]; }
static uint16_t px16(int x, int y) { return g_fb16[y * W + x]; }

/* Every channel of a and b at most `tolerance` apart */
static bool near(uint32_t a, uint32_t b, uint32_t tolerance)
{
    for (uint32_t shift = 0; shift < 32u; shift += 8u)
    {
        int32_t d = (int32_t)((a >> shift) & 0xFFu) - (int32_t)((b >> shift) & 0xFFu);
        if (d > (int32_t)tolerance || d < -(int32_t)tolerance)
        {
            Dmod_Printf("    0x%08X is not near 0x%08X\n", (unsigned)a, (unsigned)b);
            return false;
        }
    }
    return true;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
}

static bool open_fixture(const char* path)
{
    int status = 0;
    g_view = libdmview_open(path, &status);
    if (g_view == NULL)
        Dmod_Printf("    cannot open %s: %d\n", path, status);
    return g_view != NULL;
}

static bool rect_is(const libdmview_rect_t* r, int x, int y, int w, int h)
{
    if (r->x == x && r->y == y && r->w == w && r->h == h)
        return true;
    Dmod_Printf("    changed %d,%d %ux%u, expected %d,%d %dx%d\n", r->x, r->y, r->w, r->h, x, y, w, h);
    return false;
}

static void touch(int count, int x, int y, uint32_t now)
{
    dmdrvi_input_state_t state;
    memset(&state, 0, sizeof(state));
    state.contact_count = (uint8_t)count;
    state.contacts[0].x = (uint16_t)x;
    state.contacts[0].y = (uint16_t)y;
    state.contacts[0].event = DMDRVI_INPUT_CONTACT_MOVE;
    libdmview_input(g_view, &state, now);
}

/* ---- The interpreter's operand offsets are the format's ---- */

DMOD_TEST_STEP(libdmview_operand_offsets_match_the_format)
{
    int checked = 0;
    for (int op = 0; op < DMV_OPCODE_TABLE_SIZE; op++)
    {
        const dmv_opcode_info_t* info = dmv_get_opcode_info((uint8_t)op);
        dmv_layout_t layout;
        if (info == NULL || !dmv_get_layout((uint8_t)op, &layout))
            continue;
        for (unsigned i = 0; i < info->operand_count; i++, checked++)
        {
            if (layout_offset((uint8_t)op, i) != layout.offsets[i])
            {
                Dmod_Printf("    %s operand %u: %u, format %u\n", info->mnemonic, i, layout_offset((uint8_t)op, i),
                            layout.offsets[i]);
                DMOD_TEST_FAIL();
            }
        }
    }
    DMOD_TEST_EXPECT_TRUE(checked > 50);
}

/* ---- Drawing ---- */

DMOD_TEST_STEP(libdmview_draws_shapes_argb8888)
{
    libdmview_rect_t changed;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("shapes.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 0, 0, W, H));

    DMOD_TEST_EXPECT_EQ(px(5, 5), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(15, 12), 0xFFFF0000u);
    DMOD_TEST_EXPECT_EQ(px(29, 19), 0xFFFF0000u);
    DMOD_TEST_EXPECT_EQ(px(30, 19), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(0, 47), 0xFF00FF00u);        /* clipped at the edges */
    DMOD_TEST_EXPECT_EQ(px(14, 40), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(15, 40), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(45, 5), 0xFF808080u);        /* 50 % white over black */
    DMOD_TEST_EXPECT_EQ(px(10, 30), 0xFF0000FFu);       /* rounded: center filled ... */
    DMOD_TEST_EXPECT_EQ(px(0, 20), 0xFF000000u);        /* ... corner not */
    DMOD_TEST_EXPECT_EQ(px(40, 30), 0xFFFFFF00u);       /* frame */
    DMOD_TEST_EXPECT_EQ(px(41, 21), 0xFFFFFF00u);
    DMOD_TEST_EXPECT_EQ(px(45, 30), 0xFF000000u);       /* inside the frame */

    /* Nothing changed - nothing drawn */
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 0);
    DMOD_TEST_EXPECT_EQ(changed.w, 0);
}

DMOD_TEST_STEP(libdmview_draws_shapes_rgb565)
{
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("shapes.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s16, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px16(5, 5), 0x0000);
    DMOD_TEST_EXPECT_EQ(px16(15, 12), 0xF800);
    DMOD_TEST_EXPECT_EQ(px16(29, 19), 0xF800);
    DMOD_TEST_EXPECT_EQ(px16(30, 19), 0x0000);
    DMOD_TEST_EXPECT_EQ(px16(0, 47), 0x07E0);
    DMOD_TEST_EXPECT_EQ(px16(45, 5), 0x8410);           /* 50 % white */
    DMOD_TEST_EXPECT_EQ(px16(10, 30), 0x001F);

    libdmview_surface_t bad = g_s16;
    bad.format = DMDRVI_GFX_PIXEL_FORMAT_ARGB4444;
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &bad, NULL), -ENOTSUP);
}

DMOD_TEST_STEP(libdmview_draws_text)
{
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("text.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    /* 'A', first row 0x0C: pixels 2 and 3 */
    DMOD_TEST_EXPECT_EQ(px(1, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(2, 0), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(3, 0), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(4, 0), 0xFF000000u);
    /* ... twice as large at y 16: pixels 4..7, two lines */
    DMOD_TEST_EXPECT_EQ(px(3, 16), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(4, 16), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(7, 17), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(8, 16), 0xFF000000u);
}

/* ---- Box opacity ---- */

DMOD_TEST_STEP(libdmview_draws_translucent_boxes)
{
    libdmview_rect_t changed;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("opacity.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    DMOD_TEST_EXPECT_TRUE(near(px(8, 8), 0xFF808080u, 1));         /* 50 % white */
    DMOD_TEST_EXPECT_TRUE(near(px(17, 1), 0xFF800000u, 1));        /* 50 % red ... */
    DMOD_TEST_EXPECT_TRUE(near(px(24, 8), 0xFF604000u, 2));        /* ... 25 % green over it */
    DMOD_TEST_EXPECT_TRUE(near(px(63, 8), 0xFF404040u, 3));        /* A gradient at 25 % */
    DMOD_TEST_EXPECT_TRUE(near(px(33, 8), 0xFF000000u, 3));
    DMOD_TEST_EXPECT_EQ(px(8, 24), 0xFF000000u);                    /* OPACITY 0 */
    DMOD_TEST_EXPECT_TRUE(near(px(19, 16), 0xFF808080u, 1));       /* Text at 50 %: 'W' column 1 */

    /* The opacity from a variable: the box is redrawn with what lies beneath
     * it - again and again without adding up */
    for (int i = 0; i < 3; i++)
    {
        DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "fade", 255), 0);
        DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
        DMOD_TEST_EXPECT_TRUE(changed.x <= 16 && changed.x + changed.w >= 32 && changed.y == 0 && changed.h >= 16);
        DMOD_TEST_EXPECT_EQ(px(17, 1), 0xFFFF0000u);
        DMOD_TEST_EXPECT_TRUE(near(px(24, 8), 0xFF7F8000u, 2));    /* 50 % green over red */
        DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "fade", 128), 0);
        DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
        DMOD_TEST_EXPECT_TRUE(near(px(17, 1), 0xFF800000u, 1));
        DMOD_TEST_EXPECT_TRUE(near(px(24, 8), 0xFF604000u, 2));
    }
    DMOD_TEST_EXPECT_TRUE(near(px(8, 8), 0xFF808080u, 1));         /* The neighbors untouched */

    /* RGB565: a translucent gradient is blended, not dithered */
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s16, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px16(8, 8), rgb565(px(8, 8)));
    DMOD_TEST_EXPECT_EQ(px16(40, 4), px16(40, 5));
}

/* ---- Fonts ---- */

/* What the text in x0..x1 x y0..y1 lit: its width (to the rightmost lit
 * column, from x0), and how many pixels are partly lit */
static int32_t lit_width(int x0, int y0, int x1, int y1, uint32_t* partial)
{
    int32_t right = 0;
    *partial = 0;
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            uint32_t g = px(x, y) & 0xFFu;
            if (g != 0 && x - x0 + 1 > right)
                right = x - x0 + 1;
            *partial += (g != 0 && g != 0xFF) ? 1u : 0u;
        }
    }
    return right;
}

DMOD_TEST_STEP(libdmview_draws_font_files)
{
    uint32_t partial;
    Dmod_SetEnv("DMVIEW_FONTS", LIBDMVIEW_FONTS_DIR, 1);
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("fonts.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    /* Roboto: antialiased, proportional - "ii" much narrower than "WW" */
    int32_t ww = lit_width(0, 0, 32, 16, &partial);
    DMOD_TEST_EXPECT_TRUE(partial > 20u);
    int32_t ii = lit_width(0, 16, 32, 32, &partial);
    DMOD_TEST_EXPECT_TRUE(ww > 20 && ww < 32);
    DMOD_TEST_EXPECT_TRUE(ii > 0 && ii * 3 < ww);

    /* Glyphs keep to their outlines: the ':' after a 't' shows */
    DMOD_TEST_EXPECT_TRUE(lit_width(48, 0, 64, 16, &partial) >= lit_width(36, 0, 48, 16, &partial) + 2);

    /* UTF-8 beyond ASCII: two glyphs of their own, not the '?' fallback */
    int32_t pl = lit_width(32, 16, 64, 32, &partial);
    DMOD_TEST_EXPECT_TRUE(pl > 10);
    DMOD_TEST_EXPECT_TRUE(lit_width(32, 16, 48, 22, &partial) > 0);      /* The accents above them */

    /* builtin-16, and a font without a file: the built-in font, 2x - fixed
     * width (the second 'W' ends at its 7th column), nothing partly lit */
    DMOD_TEST_EXPECT_EQ(lit_width(0, 32, 32, 48, &partial), 30);
    DMOD_TEST_EXPECT_EQ(partial, 0u);
    DMOD_TEST_EXPECT_EQ(lit_width(32, 32, 64, 48, &partial), 30);
    DMOD_TEST_EXPECT_EQ(partial, 0u);

    /* A second view shares the loaded font; it stays while one view uses it */
    static uint32_t first[W * H];
    memcpy(first, g_fb32, sizeof(first));
    int status = 0;
    libdmview_t second = libdmview_open(FIXTURE("fonts.dmv"), &status);
    DMOD_TEST_EXPECT_NOT_NULL(second);
    libdmview_close(g_view);
    g_view = second;
    memset(g_fb32, 0, sizeof(g_fb32));
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    uint32_t differ = 0;
    for (int i = 0; i < W * H; i++)
        differ += (first[i] != g_fb32[i]) ? 1u : 0u;
    DMOD_TEST_EXPECT_EQ(differ, 0u);
}

DMOD_TEST_STEP(libdmview_falls_back_from_broken_fonts)
{
    uint32_t partial;
    /* A "sans-16.dmvf" that is no font */
    void* f = Dmod_FileOpen(FIXTURE("sans-16.dmvf"), "wb");
    DMOD_TEST_EXPECT_NOT_NULL(f);
    if (f == NULL)
        return;
    static const char junk[] = "DMVF this is not a font file at all, not even its header";
    Dmod_FileWrite(junk, 1, sizeof(junk), f);
    Dmod_FileClose(f);

    Dmod_SetEnv("DMVIEW_FONTS", LIBDMVIEW_FIXTURES_DIR, 1);
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("fonts.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_EQ(lit_width(0, 0, 32, 16, &partial), 30);     /* "WW", built-in, 2x */
    DMOD_TEST_EXPECT_EQ(partial, 0u);
}

DMOD_TEST_STEP(libdmview_finds_fonts_next_to_the_view)
{
    uint32_t partial;
    /* sans-16.dmvf next to fonts.dmv - $DMVIEW_FONTS is not set */
    void* in = Dmod_FileOpen(LIBDMVIEW_FONTS_DIR "/sans-16.dmvf", "rb");
    void* out = Dmod_FileOpen(FIXTURE("sans-16.dmvf"), "wb");
    DMOD_TEST_EXPECT_NOT_NULL(in);
    DMOD_TEST_EXPECT_NOT_NULL(out);
    if (in != NULL && out != NULL)
    {
        static uint8_t buffer[512];
        size_t n;
        while ((n = Dmod_FileRead(buffer, 1, sizeof(buffer), in)) > 0)
            Dmod_FileWrite(buffer, 1, n, out);
    }
    if (in != NULL)
        Dmod_FileClose(in);
    if (out != NULL)
        Dmod_FileClose(out);

    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("fonts.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    int32_t ww = lit_width(0, 0, 32, 16, &partial);                    /* Roboto, not the built-in font */
    DMOD_TEST_EXPECT_TRUE(ww > 20 && ww < 32);
    DMOD_TEST_EXPECT_TRUE(partial > 20u);
}

/* ---- FORMAT ---- */

/* Rows y0 .. y0+8 of x0 .. x1 are lit and the same as the 8 rows below them */
static bool same_as_below(int x0, int x1, int y0)
{
    uint32_t lit = 0;
    for (int y = y0; y < y0 + 8; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            if (px(x, y) != px(x, y + 8))
                return false;
            lit += (px(x, y) != 0xFF000000u) ? 1u : 0u;
        }
    }
    return lit > 0;
}

DMOD_TEST_STEP(libdmview_formats_with_padding)
{
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("format.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_TRUE(same_as_below(0, 64, 0));     /* "%02d:%%02d" 7, then 5: "07:05" */
    DMOD_TEST_EXPECT_TRUE(same_as_below(0, 32, 16));    /* "%04x" 171: "00ab" */
    DMOD_TEST_EXPECT_TRUE(same_as_below(32, 64, 16));   /* "%03d" -5: "-05" */
    DMOD_TEST_EXPECT_TRUE(same_as_below(0, 64, 32));    /* "%4d" -3: "  -3" */

    uint16_t w = 0, h = 0;
    DMOD_TEST_EXPECT_EQ(libdmview_get_size(g_view, &w, &h), 0);
    DMOD_TEST_EXPECT_EQ(w, 64);
    DMOD_TEST_EXPECT_EQ(h, 48);
    DMOD_TEST_EXPECT_EQ(libdmview_get_size(NULL, &w, &h), -EINVAL);
}

DMOD_TEST_STEP(libdmview_appends_strings)
{
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("append.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_TRUE(same_as_below(0, 64, 0));     /* "68" " " "km" "/h": "68 km/h" */
    DMOD_TEST_EXPECT_TRUE(same_as_below(0, 64, 16));    /* "ab" + "cdef" into str[4]: "abcd" */
}

/* ---- Antialiasing ---- */


/* Coverage of white on black in x0..x1 x y0..y1, in pixels */
static uint32_t white_area(int x0, int y0, int x1, int y1)
{
    uint32_t sum = 0;
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
            sum += px(x, y) & 0xFFu;
    }
    return (sum + 127u) / 255u;
}

static bool between(uint32_t value, uint32_t low, uint32_t high)
{
    if (value >= low && value <= high)
        return true;
    Dmod_Printf("    %u is not in %u ... %u\n", (unsigned)value, (unsigned)low, (unsigned)high);
    return false;
}

DMOD_TEST_STEP(libdmview_antialiases_curves)
{
    libdmview_rect_t changed;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("aa.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    /* Circle r 12: covers pi * 144 = 452.4 pixels, edges partly */
    DMOD_TEST_EXPECT_TRUE(between(white_area(0, 0, 32, 32), 450, 455));
    DMOD_TEST_EXPECT_EQ(px(16, 16), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(2, 16), 0xFF000000u);
    uint32_t partial = 0;
    bool symmetric = true;
    for (int y = 0; y < 32; y++)
    {
        for (int x = 0; x < 32; x++)
        {
            uint32_t g = px(x, y) & 0xFFu;
            partial += (g != 0 && g != 0xFF) ? 1u : 0u;
            symmetric = symmetric && px(x, y) == px(31 - x, y) && px(x, y) == px(x, 31 - y);
        }
    }
    DMOD_TEST_EXPECT_TRUE(partial > 40u);
    DMOD_TEST_EXPECT_TRUE(symmetric);

    /* Ring r 12, 3 thick: pi * (144 - 81) = 197.9, the hole black */
    DMOD_TEST_EXPECT_TRUE(between(white_area(32, 0, 64, 30), 195, 201));
    DMOD_TEST_EXPECT_EQ(px(48, 16), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(37, 16), 0xFFFFFFFFu);

    /* Rounded rectangle: straight edges stay sharp, the corners are smooth;
     * 24 * 14 - (4 - pi) * 36 = 305.1 */
    DMOD_TEST_EXPECT_TRUE(between(white_area(0, 30, 32, 48), 303, 307));
    DMOD_TEST_EXPECT_EQ(px(4, 39), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(3, 39), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(16, 32), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(16, 31), 0xFF000000u);
    uint32_t corner = px(5, 33) & 0xFFu;
    DMOD_TEST_EXPECT_TRUE(corner != 0 && corner != 0xFF);

    /* Redrawing the transparent box draws what is beneath first: its edges
     * are blended once, as on the first draw */
    static uint32_t first[W * H];
    memcpy(first, g_fb32, sizeof(first));
    DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "n", 1), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 34, 30, 28, 18));
    uint32_t redrawn_differ = 0;
    for (int i = 0; i < W * H; i++)
        redrawn_differ += (first[i] != g_fb32[i]) ? 1u : 0u;
    DMOD_TEST_EXPECT_EQ(redrawn_differ, 0u);

    /* RGB565: the same edges */
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s16, NULL), 1);
    uint32_t differ = 0;
    for (int i = 0; i < W * H; i++)
        differ += (g_fb16[i] != rgb565(g_fb32[i])) ? 1u : 0u;
    DMOD_TEST_EXPECT_EQ(differ, 0u);
}

/* ---- Gradients ---- */


DMOD_TEST_STEP(libdmview_draws_gradients)
{
    libdmview_rect_t changed;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("gradients.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    /* Linear, down: one color per line, black to white over the 32 lines */
    DMOD_TEST_EXPECT_TRUE(near(px(0, 0), 0xFF000000u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(0, 16), 0xFF808080u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(15, 31), 0xFFFFFFFFu, 8));
    DMOD_TEST_EXPECT_EQ(px(0, 10), px(15, 10));
    DMOD_TEST_EXPECT_TRUE(px(0, 10) != px(0, 11));

    /* Linear, right: red to blue, one color per column */
    DMOD_TEST_EXPECT_TRUE(near(px(16, 4), 0xFFFF0000u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(32, 4), 0xFF800080u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(47, 4), 0xFF0000FFu, 8));
    DMOD_TEST_EXPECT_EQ(px(20, 0), px(20, 7));

    /* Radial: white in the middle of the circle, darker towards its edge,
     * the same at the same distance */
    DMOD_TEST_EXPECT_TRUE(near(px(56, 8), 0xFFFFFFFFu, 24));
    DMOD_TEST_EXPECT_TRUE(near(px(49, 8), 0xFF000000u, 64));
    DMOD_TEST_EXPECT_EQ(px(53, 8), px(56, 5));
    DMOD_TEST_EXPECT_TRUE((px(52, 8) & 0xFFu) < (px(54, 8) & 0xFFu));

    /* Transparent to white, blended over black */
    DMOD_TEST_EXPECT_TRUE(near(px(16, 12), 0xFF000000u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(32, 12), 0xFF808080u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(47, 12), 0xFFFFFFFFu, 8));

    /* A hard stop: red up to the middle, green after it */
    DMOD_TEST_EXPECT_EQ(px(31, 20), 0xFFFF0000u);
    DMOD_TEST_EXPECT_EQ(px(32, 20), 0xFF00FF00u);

    /* FILL spans its box: black at the panel's top, white at its bottom */
    DMOD_TEST_EXPECT_TRUE(near(px(20, 24), 0xFF000000u, 8));
    DMOD_TEST_EXPECT_TRUE(near(px(20, 47), 0xFFFFFFFFu, 8));

    /* Redrawing the transparent @mark redraws the panel beneath it, clipped
     * to the mark - the gradient still spans the whole panel */
    uint32_t before = px(30, 38);
    DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "mark", 1), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 24, 32, 8, 8));
    DMOD_TEST_EXPECT_EQ(px(24, 32), 0xFFFF00FFu);
    DMOD_TEST_EXPECT_EQ(px(30, 38), before);
    DMOD_TEST_EXPECT_EQ(px(30, 38), px(60, 38));

    /* RGB565: the opaque gradients are the same colors, converted */
    DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "mark", 0), 0);
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s16, NULL), 1);
    /* Dithered: every pixel at most one RGB565 step from the color, and the
     * 4x4 blocks average to it closer than cutting the bits off does */
    uint32_t differ = 0;
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            bool fade = x >= 16 && x < 48 && y >= 8 && y < 16;
            uint16_t a = px16(x, y), b = rgb565(px(x, y));
            int dr = (a >> 11) - (b >> 11), dg = ((a >> 5) & 0x3F) - ((b >> 5) & 0x3F), db = (a & 0x1F) - (b & 0x1F);
            if (!fade && (dr < -1 || dr > 1 || dg < -1 || dg > 1 || db < -1 || db > 1))
                differ++;
        }
    }
    DMOD_TEST_EXPECT_EQ(differ, 0u);

    /* "down", black to white over 32 lines: the red of every 4x4 block */
    uint32_t dithered_error = 0, cut_error = 0, varied_rows = 0;
    for (int by = 0; by < 32; by += 4)
    {
        for (int bx = 0; bx < 16; bx += 4)
        {
            int32_t want = 0, dithered = 0, cut = 0;
            for (int y = by; y < by + 4; y++)
            {
                for (int x = bx; x < bx + 4; x++)
                {
                    want += (int32_t)((px(x, y) >> 16) & 0xFFu);
                    dithered += (int32_t)((px16(x, y) >> 11) * 255u / 31u);
                    cut += (int32_t)((rgb565(px(x, y)) >> 11) * 255u / 31u);
                }
            }
            dithered_error += (uint32_t)((dithered > want) ? dithered - want : want - dithered);
            cut_error += (uint32_t)((cut > want) ? cut - want : want - cut);
        }
    }
    for (int y = 0; y < 32; y++)
        varied_rows += (px16(0, y) != px16(1, y) || px16(1, y) != px16(2, y)) ? 1u : 0u;
    DMOD_TEST_EXPECT_TRUE(dithered_error * 3u < cut_error);
    DMOD_TEST_EXPECT_TRUE(varied_rows > 8u);        /* A vertical gradient's lines are patterns */
    DMOD_TEST_EXPECT_EQ(px16(32, 12) & 0xF800u, 0x8000u);    /* Blended: half white */
}

static uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t* p) { return (uint32_t)rd16le(p) | ((uint32_t)rd16le(p + 2) << 16); }

static uint8_t g_view_bytes[4096];
static uint32_t g_view_size;

static int bytes_read(void* ctx, uint32_t offset, void* buffer, size_t size)
{
    (void)ctx;
    if (offset + size > g_view_size)
        return -EIO;
    memcpy(buffer, g_view_bytes + offset, size);
    return 0;
}

DMOD_TEST_STEP(libdmview_rejects_invalid_gradients)
{
    void* f = Dmod_FileOpen(FIXTURE("gradients.dmv"), "r");
    DMOD_TEST_EXPECT_NOT_NULL(f);
    if (f == NULL)
        return;
    g_view_size = (uint32_t)Dmod_FileRead(g_view_bytes, 1, sizeof(g_view_bytes), f);
    Dmod_FileClose(f);

    dmv_input_t in;
    in.read = bytes_read;
    in.ctx = NULL;
    in.size = g_view_size;
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_VALID);
    DMOD_TEST_EXPECT_EQ(rd16le(g_view_bytes + 6), 2);

    /* RECT 0, 0, 16, 32, down: the second instruction, its gradient index at 12 */
    uint8_t* code = g_view_bytes + rd32le(g_view_bytes + 24);
    uint8_t* rect = code + code[1];
    DMOD_TEST_EXPECT_TRUE(rect[0] == DMV_OP_RECT && rect[3] == DMV_PAINT_GRADIENT);
    rect[12] = 9;                                           /* No gradient 9 */
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_ERR_OPERAND);
    rect[12] = 0;
    rect[2] = 0x10;                                         /* A gradient from a variable */
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_ERR_OPERAND);
    rect[2] = 0;

    /* OPACITY anywhere but right after BOX (SCROLL, FOCUS) */
    uint8_t first[8];
    memcpy(first, code, sizeof(first));
    code[0] = DMV_OP_OPACITY;                               /* The first FILL (8 bytes, too) */
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_ERR_NESTING);
    memcpy(code, first, sizeof(first));
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_VALID);

    /* A stop before the previous one */
    uint8_t* stops = g_view_bytes + rd32le(g_view_bytes + 88);
    stops[4] = 0xE8; stops[5] = 0x03;                       /* 1000, then 1000 - still in order */
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_VALID);
    stops[12] = 0; stops[13] = 0;                           /* 1000, then 0 */
    DMOD_TEST_EXPECT_EQ(libdmview_validate(&in, NULL), DMV_ERR_TABLE);
}

/* ---- Redrawing what changed, input ---- */

DMOD_TEST_STEP(libdmview_redraws_only_what_changed)
{
    libdmview_rect_t changed;
    int32_t value = -1;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("ui.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_EQ(px(15, 15), 0xFF0000FFu);
    DMOD_TEST_EXPECT_EQ(px(45, 15), 0xFFFF0000u);

    /* Pressing the button redraws the button only, darker */
    touch(1, 15, 15, 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 10, 10, 20, 10));
    DMOD_TEST_EXPECT_EQ(px(15, 15), 0xFF000080u);

    /* Dragging: $ev.x is relative to the box */
    touch(1, 20, 15, 10);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "drag_x", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 10);

    /* Releasing inside clicks */
    touch(0, 0, 0, 20);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "count", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 10, 10, 20, 10));
    DMOD_TEST_EXPECT_EQ(px(15, 15), 0xFF0000FFu);

    /* A variable only the label reads */
    DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "level", 1), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 40, 10, 20, 10));
    DMOD_TEST_EXPECT_EQ(px(45, 15), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 0);

    /* Setting the same value again invalidates nothing */
    DMOD_TEST_EXPECT_EQ(libdmview_set_int(g_view, "level", 1), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 0);

    /* Releasing outside does not click */
    touch(1, 15, 15, 30);
    touch(1, 50, 40, 40);
    touch(0, 0, 0, 50);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "count", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);

    /* Touching nowhere in particular */
    touch(1, 60, 45, 60);
    touch(0, 0, 0, 70);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "count", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);

    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "nope", &value), -ENOENT);
    DMOD_TEST_EXPECT_EQ(libdmview_set_string(g_view, "count", "x"), -EINVAL);    /* not a string */
}

DMOD_TEST_STEP(libdmview_runs_long_press_and_timers)
{
    int32_t value = 0;
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("ui.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    /* .timer 100: due 100 ms after the view's first moment */
    DMOD_TEST_EXPECT_EQ(libdmview_update(g_view, 1000), 100u);
    DMOD_TEST_EXPECT_EQ(libdmview_update(g_view, 1099), 1u);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "ticks", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 0);
    DMOD_TEST_EXPECT_EQ(libdmview_update(g_view, 1100), 100u);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "ticks", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);

    /* .longpress 500 */
    touch(1, 15, 15, 1150);
    DMOD_TEST_EXPECT_EQ(libdmview_update(g_view, 1150), 50u);       /* the timer comes first */
    (void)libdmview_update(g_view, 1600);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "long", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 0);
    (void)libdmview_update(g_view, 1650);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "long", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);
    touch(0, 0, 0, 1700);
}

DMOD_TEST_STEP(libdmview_env_variables_and_goto)
{
    int32_t value = 0;
    DMOD_TEST_EXPECT_EQ(Dmod_SetEnv("DMVIEW_TEST_LEVEL", "7", 1), 0);
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("actions.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "env", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 7);                       /* taken over from dmenv */
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_NULL(libdmview_take_goto(g_view));

    touch(1, 5, 5, 0);
    touch(0, 0, 0, 10);
    const char* env = Dmod_GetEnv("DMVIEW_TEST_LEVEL");
    DMOD_TEST_EXPECT_TRUE(env != NULL && strcmp(env, "42") == 0);  /* written back */
    const char* next = libdmview_take_goto(g_view);
    DMOD_TEST_EXPECT_TRUE(next != NULL && strcmp(next, "/views/next.dmv") == 0);
    DMOD_TEST_EXPECT_NULL(libdmview_take_goto(g_view));
}

/* ---- Invalid views ---- */

static uint8_t g_junk[96];

static int junk_read(void* ctx, uint32_t offset, void* buffer, size_t size)
{
    (void)ctx;
    if (offset + size > sizeof(g_junk))
        return -EIO;
    memcpy(buffer, g_junk + offset, size);
    return 0;
}

DMOD_TEST_STEP(libdmview_rejects_invalid_views)
{
    int status = 0;
    dmv_input_t in;
    memset(g_junk, 0, sizeof(g_junk));
    g_junk[0] = 'D'; g_junk[1] = 'M'; g_junk[2] = 'V';
    in.read = junk_read;
    in.ctx = NULL;
    in.size = sizeof(g_junk);

    DMOD_TEST_EXPECT_NE(libdmview_validate(&in, NULL), DMV_VALID);
    DMOD_TEST_EXPECT_NULL(libdmview_open_input(&in, &status));
    DMOD_TEST_EXPECT_EQ(status, -EBADMSG);
    DMOD_TEST_EXPECT_NULL(libdmview_open(FIXTURE("missing.dmv"), &status));
    DMOD_TEST_EXPECT_EQ(status, -ENOENT);
    DMOD_TEST_EXPECT_EQ(libdmview_render(NULL, &g_s32, NULL), -EINVAL);
}

/* ---- Displays and claims ---- */

static int top_of(libdmview_display_t d, char* path, size_t size)
{
    size_t length = 0;
    return libdmview_display_view(d, NULL, path, size, &length);
}

DMOD_TEST_STEP(libdmview_claims_stack_per_display)
{
    char path[32];
    uint32_t a = 0, b = 0, c = 0, generation = 0;
    size_t length = 99;

    libdmview_display_t first = libdmview_display_register("first");
    libdmview_display_t second = libdmview_display_register("second");
    DMOD_TEST_EXPECT_NOT_NULL(first);
    DMOD_TEST_EXPECT_NOT_NULL(second);
    DMOD_TEST_EXPECT_NULL(libdmview_display_register("first"));      /* taken */

    DMOD_TEST_EXPECT_EQ(libdmview_display_view(first, &generation, path, sizeof(path), &length), -ENOENT);
    DMOD_TEST_EXPECT_EQ(length, 0u);
    uint32_t before = generation;

    /* NULL: the first registered display */
    DMOD_TEST_EXPECT_EQ(libdmview_claim(NULL, "/a.dmv", &a), 0);
    DMOD_TEST_EXPECT_NE(libdmview_display_generation(first), before);
    DMOD_TEST_EXPECT_EQ(top_of(first, path, sizeof(path)), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(path, "/a.dmv"), 0);
    DMOD_TEST_EXPECT_EQ(top_of(second, path, sizeof(path)), -ENOENT);

    DMOD_TEST_EXPECT_EQ(libdmview_claim("second", "/b.dmv", &b), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_claim("first", "/c.dmv", &c), 0);
    DMOD_TEST_EXPECT_EQ(top_of(first, path, sizeof(path)), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(path, "/c.dmv"), 0);                  /* newest wins */
    DMOD_TEST_EXPECT_EQ(libdmview_display_view(first, NULL, path, 3, &length), -ERANGE);
    DMOD_TEST_EXPECT_EQ(length, strlen("/c.dmv"));

    DMOD_TEST_EXPECT_EQ(libdmview_release(c), 0);
    DMOD_TEST_EXPECT_EQ(top_of(first, path, sizeof(path)), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(path, "/a.dmv"), 0);                  /* back to the previous one */
    DMOD_TEST_EXPECT_EQ(libdmview_release(a), 0);
    DMOD_TEST_EXPECT_EQ(top_of(first, path, sizeof(path)), -ENOENT); /* back to the service's view */
    DMOD_TEST_EXPECT_EQ(libdmview_release(a), -ENOENT);

    DMOD_TEST_EXPECT_EQ(libdmview_claim("nope", "/x.dmv", &a), -ENODEV);

    /* Unregistering drops the display's claims */
    libdmview_display_unregister(second);
    DMOD_TEST_EXPECT_EQ(libdmview_release(b), -ENOENT);
    libdmview_display_unregister(first);
    DMOD_TEST_EXPECT_EQ(libdmview_claim(NULL, "/a.dmv", &a), -ENODEV);
}

/* ---- Images ---- */

/* A .dmvi file (docs/image-format.md): `rows` rows of `row` bytes of
 * pixels, stored `stride` apart, then the alpha plane, then the palette */
static bool write_image(const char* name, uint8_t format, uint16_t w, uint16_t h, const void* pixels, uint32_t row,
                        uint32_t stride, const uint8_t* alpha, const uint32_t* palette, uint16_t colors)
{
    static uint8_t file[4096];
    uint32_t at = sizeof(dmvi_header_t), alpha_at = 0, alpha_stride = 0, palette_at = 0;

    memset(file, 0, sizeof(file));
    for (uint32_t y = 0; y < h; y++)
        memcpy(file + at + y * stride, (const uint8_t*)pixels + y * row, row);
    uint32_t end = (at + stride * h + 3U) & ~3U;
    if (alpha != NULL)
    {
        alpha_at = end;
        alpha_stride = (w + 3U) & ~3U;
        for (uint32_t y = 0; y < h; y++)
            memcpy(file + alpha_at + y * alpha_stride, alpha + y * w, w);
        end = alpha_at + alpha_stride * h;
    }
    if (palette != NULL)
    {
        palette_at = end;
        memcpy(file + palette_at, palette, colors * 4U);
        end = palette_at + colors * 4U;
    }

    dmvi_header_t hd;
    memset(&hd, 0, sizeof(hd));
    memcpy(hd.magic, "DMVI", 4);
    hd.version_major = DMVI_VERSION_MAJOR;
    hd.version_minor = DMVI_VERSION_MINOR;
    hd.file_size = end;
    hd.width = w;
    hd.height = h;
    hd.format = format;
    hd.palette_count = (palette != NULL) ? colors : 0;
    hd.stride = stride;
    hd.pixels = at;
    hd.alpha_stride = alpha_stride;
    hd.alpha = alpha_at;
    hd.palette = palette_at;
    hd.unpacked_size = end - (uint32_t)sizeof(hd);
    memcpy(file, &hd, sizeof(hd));

    void* f = Dmod_FileOpen(name, "wb");
    if (f == NULL)
        return false;
    bool ok = Dmod_FileWrite(file, 1, end, f) == end;
    Dmod_FileClose(f);
    return ok;
}

/* Compress what follows the header of an image file with dmod's compression `name` */
static bool compress_image(const char* path, const char* name)
{
    static uint8_t file[4096], packed[4096 + 512];
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return false;
    size_t size = Dmod_FileRead(file, 1, sizeof(file), f);
    Dmod_FileClose(f);

    dmvi_header_t hd;
    memcpy(&hd, file, sizeof(hd));
    size_t n = Dmod_Compression_Pack("fastlz", 1, packed, sizeof(packed), file + sizeof(hd), size - sizeof(hd));
    if (n == 0)
        return false;
    memset(hd.compression, 0, sizeof(hd.compression));
    strcpy(hd.compression, name);
    hd.file_size = (uint32_t)(sizeof(hd) + n);
    if ((f = Dmod_FileOpen(path, "wb")) == NULL)
        return false;
    bool ok = Dmod_FileWrite(&hd, 1, sizeof(hd), f) == sizeof(hd) && Dmod_FileWrite(packed, 1, n, f) == n;
    Dmod_FileClose(f);
    return ok;
}

static bool write_rgb565(uint16_t color)
{
    uint16_t px[16];
    for (int i = 0; i < 16; i++)
        px[i] = color;
    px[1 * 4 + 1] = 0x07E0;                                     /* (1, 1) green */
    return write_image(FIXTURE("rgb565.dmvi"), DMVI_FORMAT_RGB565, 4, 4, px, 8, 8, NULL, NULL, 0);
}

static bool write_images(void)
{
    uint32_t argb[16];
    for (int i = 0; i < 16; i++)
        argb[i] = (i % 4 < 2) ? 0x80FFFFFFu : 0xFF0000FFu;     /* 50 % white | blue */
    uint8_t i8[16] = { 0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 5 };
    uint32_t palette[2] = { 0xFF00FF00u, 0x00000000u };         /* green, transparent - 5 is missing */
    uint16_t white[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    uint8_t white_alpha[4] = { 255, 0, 128, 255 };
    uint8_t mask[16];
    memset(mask, 255, sizeof(mask));
    mask[0] = 128;
    uint8_t a4[4] = { 0xF0, 0x08, 0x0F, 0x0F };                 /* 3x2: 0 15 8 / 15 0 15 */
    uint8_t bar[64 * 4];
    memset(bar, 255, sizeof(bar));

    return write_rgb565(0xF800) &&
           write_image(FIXTURE("argb.dmvi"), DMVI_FORMAT_ARGB8888, 4, 4, argb, 16, 16, NULL, NULL, 0) &&
           write_image(FIXTURE("i8.dmvi"), DMVI_FORMAT_I8, 4, 4, i8, 4, 4, NULL, palette, 2) &&
           write_image(FIXTURE("rgb565a8.dmvi"), DMVI_FORMAT_RGB565A8, 2, 2, white, 4, 4, white_alpha, NULL, 0) &&
           write_image(FIXTURE("mask.dmvi"), DMVI_FORMAT_A8, 4, 4, mask, 4, 4, NULL, NULL, 0) &&
           write_image(FIXTURE("a4.dmvi"), DMVI_FORMAT_A4, 3, 2, a4, 2, 2, NULL, NULL, 0) &&
           write_image(FIXTURE("bar.dmvi"), DMVI_FORMAT_A8, 64, 4, bar, 64, 64, NULL, NULL, 0);
}

DMOD_TEST_STEP(libdmview_draws_images)
{
    DMOD_TEST_EXPECT_TRUE(write_images());
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("images.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    DMOD_TEST_EXPECT_EQ(px(0, 0), 0xFFFF0000u);                 /* RGB565 */
    DMOD_TEST_EXPECT_EQ(px(1, 1), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(3, 3), 0xFFFF0000u);
    DMOD_TEST_EXPECT_EQ(px(4, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(21, 6), 0xFF000000u);                /* ARGB8888, centered */
    DMOD_TEST_EXPECT_TRUE(near(px(22, 6), 0xFF808080u, 1));
    DMOD_TEST_EXPECT_EQ(px(24, 9), 0xFF0000FFu);
    DMOD_TEST_EXPECT_EQ(px(26, 6), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(44, 12), 0xFF00FF00u);               /* I8, bottom right */
    DMOD_TEST_EXPECT_EQ(px(44, 13), 0xFF000000u);               /* transparent color */
    DMOD_TEST_EXPECT_EQ(px(46, 15), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(47, 15), 0xFF000000u);               /* not in the palette */
    DMOD_TEST_EXPECT_EQ(px(43, 15), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(48, 0), 0xFFFFFFFFu);                /* RGB565A8 */
    DMOD_TEST_EXPECT_EQ(px(49, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_TRUE(near(px(48, 1), 0xFF808080u, 1));
    DMOD_TEST_EXPECT_EQ(px(49, 1), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(px(0, 17), 0xFFFF0000u);                /* clipped to its rectangle */
    DMOD_TEST_EXPECT_EQ(px(1, 17), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(2, 16), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(0, 18), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(16, 16), 0xFFFF0000u);               /* from a variable */
    DMOD_TEST_EXPECT_TRUE(near(px(32, 16), 0xFF800000u, 1));   /* in a box at 50 % */
    DMOD_TEST_EXPECT_TRUE(near(px(33, 17), 0xFF008000u, 1));
    DMOD_TEST_EXPECT_EQ(px(48, 16), 0xFF000000u);               /* no such file */
    DMOD_TEST_EXPECT_EQ(px(0, 32), 0xFF000000u);                /* a mask */

    /* Another path in the variable: the new image, only in its box */
    libdmview_rect_t changed;
    DMOD_TEST_EXPECT_EQ(libdmview_set_string(g_view, "photo", "argb.dmvi"), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 16, 16, 16, 16));
    DMOD_TEST_EXPECT_TRUE(near(px(16, 16), 0xFF808080u, 1));
    DMOD_TEST_EXPECT_EQ(px(18, 16), 0xFF0000FFu);
    DMOD_TEST_EXPECT_EQ(libdmview_set_string(g_view, "photo", ""), 0);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px(16, 16), 0xFF000000u);               /* no path, no image */

    /* RELOAD: the file changed, everything that shows it is redrawn */
    DMOD_TEST_EXPECT_TRUE(write_rgb565(0x001F));
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 0);
    DMOD_TEST_EXPECT_EQ(px(0, 0), 0xFFFF0000u);                 /* loaded once, kept */
    touch(1, 50, 40, 0);
    touch(0, 50, 40, 10);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px(0, 0), 0xFF0000FFu);
    DMOD_TEST_EXPECT_EQ(px(1, 1), 0xFF00FF00u);
    DMOD_TEST_EXPECT_TRUE(near(px(32, 16), 0xFF000080u, 1));

    /* RGB565: the opaque image is copied */
    libdmview_invalidate(g_view);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s16, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px16(0, 0), 0x001F);
    DMOD_TEST_EXPECT_EQ(px16(1, 1), 0x07E0);
    DMOD_TEST_EXPECT_EQ(px16(24, 9), 0x001F);
    DMOD_TEST_EXPECT_EQ(px16(44, 12), 0x07E0);
    DMOD_TEST_EXPECT_EQ(px16(49, 0), 0x0000);
    DMOD_TEST_EXPECT_EQ(px16(49, 1), 0xFFFF);
    DMOD_TEST_EXPECT_TRUE(write_rgb565(0xF800));
}

DMOD_TEST_STEP(libdmview_rejects_broken_images)
{
    /* The pixels run past the end of the file: not shown */
    uint16_t red[4] = { 0xF800, 0xF800, 0xF800, 0xF800 };
    DMOD_TEST_EXPECT_TRUE(write_image(FIXTURE("rgb565.dmvi"), DMVI_FORMAT_RGB565, 2, 2, red, 4, 4, NULL, NULL, 0));
    void* f = Dmod_FileOpen(FIXTURE("rgb565.dmvi"), "r+b");
    DMOD_TEST_EXPECT_TRUE(f != NULL);
    if (f != NULL)
    {
        uint8_t height[2] = { 200, 0 };
        (void)Dmod_FileSeek(f, 14, DMOD_SEEK_SET);
        DMOD_TEST_EXPECT_EQ((int)Dmod_FileWrite(height, 1, 2, f), 2);
        Dmod_FileClose(f);
    }
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("images.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_EQ(px(0, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_TRUE(near(px(22, 6), 0xFF808080u, 1));    /* the others are */
    DMOD_TEST_EXPECT_TRUE(write_rgb565(0xF800));
}

DMOD_TEST_STEP(libdmview_unpacks_compressed_images)
{
    if (!Dmod_Compression_IsSupported("fastlz"))
    {
        Dmod_Printf("    no fastlz in this dmod - skipped\n");
        return;
    }
    DMOD_TEST_EXPECT_TRUE(write_images());
    DMOD_TEST_EXPECT_TRUE(compress_image(FIXTURE("argb.dmvi"), "fastlz"));
    DMOD_TEST_EXPECT_TRUE(compress_image(FIXTURE("i8.dmvi"), "fastlz"));
    DMOD_TEST_EXPECT_TRUE(compress_image(FIXTURE("rgb565.dmvi"), "nolz"));     /* unknown: not shown */
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("images.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);
    DMOD_TEST_EXPECT_TRUE(near(px(22, 6), 0xFF808080u, 1));
    DMOD_TEST_EXPECT_EQ(px(24, 9), 0xFF0000FFu);
    DMOD_TEST_EXPECT_EQ(px(44, 12), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(44, 13), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(0, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_TRUE(write_images());
}

DMOD_TEST_STEP(libdmview_draws_icons)
{
    DMOD_TEST_EXPECT_TRUE(write_images());
    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("icons.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &g_s32, NULL), 1);

    DMOD_TEST_EXPECT_TRUE(near(px(0, 0), 0xFF800000u, 1));     /* A8, coverage 128 */
    DMOD_TEST_EXPECT_EQ(px(1, 0), 0xFFFF0000u);
    DMOD_TEST_EXPECT_EQ(px(4, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(16, 0), 0xFF000000u);                /* A4: 0 15 8 / 15 0 15 */
    DMOD_TEST_EXPECT_EQ(px(17, 0), 0xFF00FF00u);
    DMOD_TEST_EXPECT_TRUE(near(px(18, 0), 0xFF008800u, 1));
    DMOD_TEST_EXPECT_EQ(px(16, 1), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(17, 1), 0xFF000000u);
    DMOD_TEST_EXPECT_EQ(px(18, 1), 0xFF00FF00u);
    DMOD_TEST_EXPECT_EQ(px(19, 0), 0xFF000000u);
    DMOD_TEST_EXPECT_TRUE(near(px(32, 0), 0xFF000080u, 1));    /* ARGB8888's alpha */
    DMOD_TEST_EXPECT_EQ(px(34, 0), 0xFF0000FFu);
    DMOD_TEST_EXPECT_TRUE(near(px(0, 16), 0xFF000000u, 4));    /* a gradient */
    DMOD_TEST_EXPECT_TRUE(near(px(63, 19), 0xFFFFFFFFu, 4));
    DMOD_TEST_EXPECT_TRUE(near(px(32, 17), 0xFF808080u, 6));
    DMOD_TEST_EXPECT_EQ(px(54, 6), 0xFFFFFF00u);                /* RGB565: all covered */
    DMOD_TEST_EXPECT_EQ(px(53, 6), 0xFF000000u);
}

/* ---- The example of docs/assembly.md on a 480x272 RGB565 screen ---- */

#define DEMO_W  480
#define DEMO_H  272

static uint16_t g_screen[DEMO_W * DEMO_H];

DMOD_TEST_STEP(libdmview_runs_the_documented_example)
{
    libdmview_surface_t screen;
    libdmview_rect_t changed;
    int32_t value = 0;

    screen.pixels = g_screen;
    screen.width = DEMO_W;
    screen.height = DEMO_H;
    screen.stride = DEMO_W * 2;
    screen.format = DMDRVI_GFX_PIXEL_FORMAT_RGB565;

    DMOD_TEST_EXPECT_TRUE(open_fixture(FIXTURE("demo.dmv")));
    if (g_view == NULL)
        return;
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &screen, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 0, 0, DEMO_W, DEMO_H));
    DMOD_TEST_EXPECT_EQ(g_screen[5 * DEMO_W + 5], 0x10C4);              /* #101820 */

    /* The slider is not opaque: what lies beneath it (the root) is
     * redrawn, but only within the slider */
    touch(1, 16 + 60, 130 + 16, 0);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "level", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 20);                                     /* 60 * 100 / 300 */
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &screen, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 16, 130, 300, 32));
    touch(0, 0, 0, 10);
    (void)libdmview_render(g_view, &screen, &changed);

    /* Clicking the counter: its own area only */
    touch(1, 16 + 80, 60 + 24, 20);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &screen, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 16, 60, 160, 48));          /* pressed: darker */
    touch(0, 0, 0, 30);
    DMOD_TEST_EXPECT_EQ(libdmview_get_int(g_view, "count", &value), 0);
    DMOD_TEST_EXPECT_EQ(value, 1);
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &screen, &changed), 1);
    DMOD_TEST_EXPECT_TRUE(rect_is(&changed, 16, 60, 160, 48));
    DMOD_TEST_EXPECT_EQ(libdmview_render(g_view, &screen, &changed), 0);
}
