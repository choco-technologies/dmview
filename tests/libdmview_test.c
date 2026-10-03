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
}

void dmod_test_teardown(void)
{
    libdmview_close(g_view);
    g_view = NULL;
}

static uint32_t px(int x, int y) { return g_fb32[y * W + x]; }
static uint16_t px16(int x, int y) { return g_fb16[y * W + x]; }

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
