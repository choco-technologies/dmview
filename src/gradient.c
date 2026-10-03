#include "private.h"
#include "gradient_tables.h"

/*
 * Gradients. A gradient is turned into a palette of GRADIENT_STEPS colors
 * once, so drawing it only looks colors up; on RGB565 opaque gradients are
 * dithered on the way to the pixels (dither565()).
 * Placed on a shape (paint_gradient()), every pixel's palette index is a
 * linear function of x and y for a linear gradient - one addition per pixel,
 * and a vertical gradient is a single color per span - and the square root
 * of a sum of squares for a radial one, taken from a table.
 *
 * The geometry follows CSS: a linear gradient's angle points where it goes
 * to (0 = up, 90 = right), its line is as long as the shape's corners need;
 * a radial one is an ellipse of radii rx, ry around cx, cy.
 */

#define CHUNK           32              /* Pixels whose palette indices are computed at once */
#define ONE             65536           /* 1.0 in 16.16 */
#define INDEX_LIMIT     (1 << 30)       /* Start positions are clamped to this */

static int32_t sin_deg(int32_t d)
{
    d %= 360;
    if (d < 0)
        d += 360;
    if (d <= 90)
        return sin_q14[d];
    if (d <= 180)
        return sin_q14[180 - d];
    if (d <= 270)
        return -sin_q14[d - 180];
    return -sin_q14[360 - d];
}

static inline int32_t start(int64_t t)
{
    return (t < -INDEX_LIMIT) ? -INDEX_LIMIT : (t > INDEX_LIMIT) ? INDEX_LIMIT : (int32_t)t;
}

static inline uint32_t palette_index(int32_t t)
{
    int32_t i = t >> 16;
    return (i < 0) ? 0u : (i > (int32_t)GRADIENT_STEPS - 1) ? GRADIENT_STEPS - 1u : (uint32_t)i;
}

/* a + (b - a) * w / 256 for every channel of 0xAARRGGBB */
static uint32_t lerp(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t r = 0;
    for (uint32_t shift = 0; shift < 32u; shift += 8u)
    {
        uint32_t ca = (a >> shift) & 0xFFu, cb = (b >> shift) & 0xFFu;
        r |= ((ca * (256u - w) + cb * w + 128u) >> 8) << shift;
    }
    return r;
}

static void build_palette(grad_t* g, const dmv_stop_t* stops, uint8_t format)
{
    const dmv_stop_t* st = stops + g->first;
    uint32_t last = g->count - 1U, k = 0;

    for (uint32_t i = 0; i < GRADIENT_STEPS; i++)
    {
        /* Position of the middle of entry i */
        uint32_t pos = ((2U * i + 1U) * DMV_STOP_SCALE) / (2U * GRADIENT_STEPS);
        while (k < last && st[k + 1U].position <= pos)
            k++;
        uint32_t c;
        if (pos < st[0].position)
            c = st[0].color;
        else if (k == last)
            c = st[last].color;
        else
        {
            uint32_t from = st[k].position, span = st[k + 1U].position - from;
            c = lerp(st[k].color, st[k + 1U].color, ((pos - from) * 256U) / span);
        }
        g->lut[i] = c;
    }
    g->lut_format = format;
}

void paint_gradient(paint_t* paint, grad_t* grad, const dmv_stop_t* stops, uint8_t format,
                    int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (grad->lut_format != format)
        build_palette(grad, stops, format);
    paint->grad = grad;
    paint->ox = x;
    paint->oy = y;

    if (grad->kind == DMV_GRADIENT_LINEAR)
    {
        /* Index = 128 + (pixel - center) . direction / length * 256, with the
         * direction (sin, -cos) and the length |w sin| + |h cos| (Q14) */
        int64_t sn = sin_deg(grad->param[0]), cs = sin_deg(grad->param[0] + 90);
        int64_t length = ((w * sn < 0) ? -w * sn : w * sn) + ((h * cs < 0) ? -h * cs : h * cs);
        if (length == 0)
            length = 1;
        int64_t ax = (sn << 24) / length, ay = (-cs << 24) / length;
        paint->ax = (int32_t)ax;
        paint->ay = (int32_t)ay;
        /* At the middle of the pixel at the shape's origin */
        paint->a0 = start(((int64_t)128 << 16) + ((1 - (int64_t)w) * ax + (1 - (int64_t)h) * ay) / 2);
        paint->b0 = 0;
        paint->by = 0;
        return;
    }

    /* u = (x - cx) / rx, v = (y - cy) / ry, centers doubled for half pixels */
    int64_t cx2 = (int64_t)w * grad->param[0] / 50, cy2 = (int64_t)h * grad->param[1] / 50;
    int64_t rx = (int64_t)w * grad->param[2] / 100, ry = (int64_t)h * grad->param[3] / 100;
    if (rx < 1)
        rx = 1;
    if (ry < 1)
        ry = 1;
    paint->ax = (int32_t)(ONE / rx);
    paint->a0 = start(((1 - cx2) * ONE) / (2 * rx));
    paint->by = (int32_t)(ONE / ry);
    paint->b0 = start(((1 - cy2) * ONE) / (2 * ry));
    paint->ay = 0;
}

/* Palette index of one pixel */
static uint32_t pixel_index(const paint_t* paint, int32_t x, int32_t y)
{
    if (paint->grad->kind == DMV_GRADIENT_LINEAR)
        return palette_index(start(paint->a0 + (int64_t)(x - paint->ox) * paint->ax + (int64_t)(y - paint->oy) * paint->ay));
    int32_t u = start(paint->a0 + (int64_t)(x - paint->ox) * paint->ax);
    int32_t v = start(paint->b0 + (int64_t)(y - paint->oy) * paint->by);
    if (u <= -ONE || u >= ONE || v <= -ONE || v >= ONE)
        return GRADIENT_STEPS - 1U;
    uint32_t q = (((uint32_t)u * (uint32_t)u) >> 16) + (((uint32_t)v * (uint32_t)v) >> 16);
    return (q >= (uint32_t)ONE) ? GRADIENT_STEPS - 1U : sqrt_q16[q >> 6];
}

/* The 0xAARRGGBB color of one pixel - for the pixels antialiased edges blend */
uint32_t gradient_color(const paint_t* paint, uint8_t format, int32_t x, int32_t y)
{
    (void)format;
    return paint->grad->lut[pixel_index(paint, x, y)];
}

/*
 * RGB565 keeps 32 levels of red and blue, 64 of green: a gradient between
 * two close colors would show as a few wide bands. Opaque gradients are
 * written with ordered dithering instead - a threshold from a 4x4 Bayer
 * matrix decides whether a pixel rounds up to the next level, so the bands
 * become a fine pattern the eye averages into the colors between.
 */
static const uint8_t bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* Level of an 8-bit channel among `levels` - 1 steps, threshold (0 ... 254)
 * below one step; levels 31 and 63 are 255/31 and 255/63 apart, not 8 and 4 */
static inline uint32_t quantize(uint32_t v, uint32_t steps, uint32_t threshold)
{
    return (v * steps + threshold) / 255u;
}

static inline uint16_t dither565(uint32_t c, uint32_t t)
{
    uint32_t threshold = (t * 255u + 8u) / 16u;
    return (uint16_t)((quantize((c >> 16) & 0xFFu, 31u, threshold) << 11) |
                      (quantize((c >> 8) & 0xFFu, 63u, threshold) << 5) |
                      quantize(c & 0xFFu, 31u, threshold));
}

/* All pixels of the span in one palette color */
static void solid(const libdmview_surface_t* s, const grad_t* g, int32_t y, int32_t x0, int32_t x1, uint32_t index)
{
    uint32_t c = g->lut[index];
    if (!g->opaque || s->format != DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        draw_span(s, y, x0, x1, c);
        return;
    }
    /* Dithered: the line's pattern repeats every 4 pixels */
    const uint8_t* t = bayer[y & 3];
    uint16_t pattern[4] = { dither565(c, t[0]), dither565(c, t[1]), dither565(c, t[2]), dither565(c, t[3]) };
    if (pattern[0] == pattern[1] && pattern[1] == pattern[2] && pattern[2] == pattern[3])
    {
        draw_pixels(s, y, x0, x1, pattern[0]);
        return;
    }
    uint16_t* p = (uint16_t*)((uint8_t*)s->pixels + (uint32_t)y * s->stride);
    for (int32_t x = x0; x < x1; x++)
        p[x] = pattern[x & 3];
}

/* n pixels from x0 in the palette colors idx[] */
static void put(const libdmview_surface_t* s, const grad_t* g, int32_t y, int32_t x0, uint32_t n, const uint8_t* idx)
{
    uint8_t* row = (uint8_t*)s->pixels + (uint32_t)y * s->stride;
    const uint32_t* lut = g->lut;

    if (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        uint16_t* p = (uint16_t*)row + x0;
        if (g->opaque)
        {
            const uint8_t* t = bayer[y & 3];
            for (uint32_t i = 0; i < n; i++)
                p[i] = dither565(lut[idx[i]], t[((uint32_t)x0 + i) & 3u]);
        }
        else
        {
            for (uint32_t i = 0; i < n; i++)
                p[i] = blend565(p[i], lut[idx[i]]);
        }
        return;
    }
    uint32_t* p = (uint32_t*)row + x0;
    if (g->opaque)
    {
        for (uint32_t i = 0; i < n; i++)
            p[i] = lut[idx[i]];
    }
    else
    {
        for (uint32_t i = 0; i < n; i++)
            p[i] = blend8888(p[i], lut[idx[i]]);
    }
}

void gradient_span(const libdmview_surface_t* s, const paint_t* paint, int32_t y, int32_t x0, int32_t x1)
{
    const grad_t* g = paint->grad;
    int32_t n = x1 - x0;
    int32_t ax = paint->ax;
    uint8_t idx[CHUNK];

    if (n <= 0)
        return;

    if (g->kind == DMV_GRADIENT_LINEAR)
    {
        int32_t t = start(paint->a0 + (int64_t)(x0 - paint->ox) * ax + (int64_t)(y - paint->oy) * paint->ay);
        if (ax == 0)
        {
            solid(s, g, y, x0, x1, palette_index(t));      /* Vertical: one color per line */
            return;
        }
        while (n > 0)
        {
            uint32_t m = (n < CHUNK) ? (uint32_t)n : CHUNK;
            for (uint32_t i = 0; i < m; i++, t += ax)
                idx[i] = (uint8_t)palette_index(t);
            put(s, g, y, x0, m, idx);
            x0 += (int32_t)m;
            n -= (int32_t)m;
        }
        return;
    }

    int32_t v = start(paint->b0 + (int64_t)(y - paint->oy) * paint->by);
    if (v <= -ONE || v >= ONE)
    {
        solid(s, g, y, x0, x1, GRADIENT_STEPS - 1U);       /* The whole line is outside the ellipse */
        return;
    }
    uint32_t vq = ((uint32_t)v * (uint32_t)v) >> 16;
    int32_t u = start(paint->a0 + (int64_t)(x0 - paint->ox) * ax);
    while (n > 0)
    {
        uint32_t m = (n < CHUNK) ? (uint32_t)n : CHUNK;
        for (uint32_t i = 0; i < m; i++, u += ax)
        {
            uint32_t q = ((uint32_t)(u + ONE - 1) <= (uint32_t)(2 * ONE - 2))
                             ? (((uint32_t)u * (uint32_t)u) >> 16) + vq : (uint32_t)ONE;
            idx[i] = (q >= (uint32_t)ONE) ? (uint8_t)(GRADIENT_STEPS - 1U) : sqrt_q16[q >> 6];
        }
        put(s, g, y, x0, m, idx);
        x0 += (int32_t)m;
        n -= (int32_t)m;
    }
}
