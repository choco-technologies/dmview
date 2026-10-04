#include "private.h"

/*
 * Rasterizer. Everything is drawn as horizontal spans, clipped to a
 * rectangle; curves use an integer square root per sub-row, no floating point,
 * and are antialiased: the pixels their edges cross are blended by how much
 * of them the shape covers.
 * Opaque colors take a fast path (RGB565: two pixels per 32-bit store); only
 * colors with alpha below 0xFF are blended. A shape is drawn with a paint:
 * a color, or a gradient (gradient.c) - only the span knows the difference.
 */

bool draw_supported(uint8_t format)
{
    return format == DMDRVI_GFX_PIXEL_FORMAT_RGB565 || format == DMDRVI_GFX_PIXEL_FORMAT_ARGB8888;
}

/* Opaque pixels already in the surface's format (RGB565 in the low half) */
void draw_pixels(const libdmview_surface_t* s, int32_t y, int32_t x0, int32_t x1, uint32_t pixel)
{
    int32_t n = x1 - x0;
    uint8_t* row = (uint8_t*)s->pixels + (uint32_t)y * s->stride;

    if (n <= 0)
        return;
    if (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        uint16_t* p = (uint16_t*)row + x0;
        uint16_t c = (uint16_t)pixel;
        if (((uintptr_t)p & 2u) != 0)
        {
            *p++ = c;
            n--;
        }
        uint32_t* q = (uint32_t*)p;
        uint32_t pair = (uint32_t)c | ((uint32_t)c << 16);
        for (; n >= 2; n -= 2)
            *q++ = pair;
        if (n != 0)
            *(uint16_t*)q = c;
        return;
    }
    uint32_t* p = (uint32_t*)row + x0;
    for (; n > 0; n--)
        *p++ = pixel;
}

/* Spans at least this long blend RGB565 through per-channel tables */
#define BLEND_TABLE_SPAN    64

/* A span of one 0xAARRGGBB color - blended only below alpha 0xFF */
void draw_span(const libdmview_surface_t* s, int32_t y, int32_t x0, int32_t x1, uint32_t color)
{
    uint32_t a = color >> 24;
    int32_t n = x1 - x0;
    uint8_t* row = (uint8_t*)s->pixels + (uint32_t)y * s->stride;

    if (n <= 0 || a == 0)
        return;
    if (a == 255u)
    {
        draw_pixels(s, y, x0, x1, (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565) ? to_rgb565(color) : color);
        return;
    }
    if (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        uint16_t* p = (uint16_t*)row + x0;
        if (n < BLEND_TABLE_SPAN)
        {
            for (; n > 0; n--, p++)
                *p = blend565(*p, color);
            return;
        }
        /* One color over every level of each channel - the same as
         * blend565(), a pixel then costs three lookups */
        uint16_t red[32], green[64], blue[32];
        uint32_t sr = (color >> 16) & 0xFFu, sg = (color >> 8) & 0xFFu, sb = color & 0xFFu;
        for (uint32_t k = 0; k < 32U; k++)
        {
            red[k] = (uint16_t)((mix(sr, k << 3, a) & 0xF8u) << 8);
            blue[k] = (uint16_t)(mix(sb, k << 3, a) >> 3);
        }
        for (uint32_t k = 0; k < 64U; k++)
            green[k] = (uint16_t)((mix(sg, k << 2, a) & 0xFCu) << 3);
        for (; n > 0; n--, p++)
        {
            uint32_t d = *p;
            *p = (uint16_t)(red[d >> 11] | green[(d >> 5) & 0x3Fu] | blue[d & 0x1Fu]);
        }
        return;
    }
    uint32_t* p = (uint32_t*)row + x0;
    for (; n > 0; n--, p++)
        *p = blend8888(*p, color);
}

static inline void span(const libdmview_surface_t* s, int32_t y, int32_t x0, int32_t x1, const paint_t* paint)
{
    if (paint->grad == NULL)
        draw_span(s, y, x0, x1, paint->color);
    else
        gradient_span(s, paint, y, x0, x1);
}

/* A span clipped against `clip` */
static inline void clipped_span(const libdmview_surface_t* s, const rect_t* clip, int32_t y, int32_t x0, int32_t x1,
                                const paint_t* paint)
{
    if (y < clip->y0 || y >= clip->y1)
        return;
    if (x0 < clip->x0)
        x0 = clip->x0;
    if (x1 > clip->x1)
        x1 = clip->x1;
    span(s, y, x0, x1, paint);
}

static void fill(const libdmview_surface_t* s, const rect_t* clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 const paint_t* paint)
{
    rect_t r = { x0, y0, x1, y1 };
    r = rect_and(r, clip);
    for (int32_t y = r.y0; y < r.y1; y++)
        span(s, y, r.x0, r.x1, paint);
}

static uint32_t isqrt(uint32_t n)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit != 0)
    {
        if (n >= root + bit)
        {
            n -= root + bit;
            root = (root >> 1) + bit;
        }
        else
            root >>= 1;
        bit >>= 2;
    }
    return root;
}

static int32_t clamp_radius(int32_t r, int32_t w, int32_t h)
{
    int32_t limit = ((w < h) ? w : h) / 2;
    if (r > limit)
        r = limit;
    return (r < 0) ? 0 : r;
}

void draw_rect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, const paint_t* paint)
{
    fill(s, clip, x, y, x + w, y + h, paint);
}

void draw_frame(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t t,
                const paint_t* paint)
{
    if (w <= 0 || h <= 0 || t <= 0)
        return;
    if (2 * t >= w || 2 * t >= h)
    {
        fill(s, clip, x, y, x + w, y + h, paint);
        return;
    }
    fill(s, clip, x, y, x + w, y + t, paint);
    fill(s, clip, x, y + h - t, x + w, y + h, paint);
    fill(s, clip, x, y + t, x + t, y + h - t, paint);
    fill(s, clip, x + w - t, y + t, x + w, y + h - t, paint);
}

/* ---- Antialiased curves ----
 *
 * A row of pixels is sampled at SUB sub-rows; on each, the shape's edges are
 * found to 1/FRAC of a pixel. Pixels every sub-row covers completely are a
 * plain span; only the pixels an edge crosses get the part of them the shape
 * covers as alpha - one or two per side and row, except where an edge is
 * nearly horizontal. A row of a shape is one or two segments, each given by
 * its left and right edge on every sub-row (left >= right: nothing there).
 */

#define SUB         4                       /* Sub-rows per row */
#define FRAC_SHIFT  4
#define FRAC        (1 << FRAC_SHIFT)       /* Edge positions per pixel */
#define FULL        (SUB * FRAC)            /* Coverage of a whole pixel */
#define MAX_RADIUS  4095                    /* (FRAC * r)^2 fits 32 bits */

/* Sub-row k of pixel row y, in 1/FRAC pixels */
static inline int32_t sub_y(int32_t y, int32_t k)
{
    return FRAC * y + (FRAC / SUB) * k + FRAC / (2 * SUB);
}

/* Half width (1/FRAC px) of a circle of radius r (px) at d (1/FRAC px) from
 * its center; -1 when the line misses it. */
static inline int32_t half_width(int32_t r, int32_t d)
{
    uint32_t r16 = (uint32_t)(FRAC * r);
    uint32_t ad = (uint32_t)((d < 0) ? -d : d);
    if (ad >= r16)
        return -1;
    return (int32_t)isqrt(r16 * r16 - ad * ad);
}

/* Edges (1/FRAC px) of a rounded rectangle on the sub-row at ys; false when
 * the sub-row misses it. */
static bool rrect_edges(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t ys, int32_t* l, int32_t* rt)
{
    if (w <= 0 || h <= 0 || ys < FRAC * y || ys >= FRAC * (y + h))
        return false;
    int32_t inset = 0, top = FRAC * (y + r), bottom = FRAC * (y + h - r);
    int32_t d = (ys < top) ? top - ys : (ys > bottom) ? ys - bottom : 0;
    if (d > 0)
    {
        int32_t hw = half_width(r, d);
        inset = (hw < 0) ? FRAC * r : FRAC * r - hw;
    }
    *l = FRAC * x + inset;
    *rt = FRAC * (x + w) - inset;
    return *rt > *l;
}

static bool circle_edges(int32_t cx, int32_t cy, int32_t r, int32_t ys, int32_t* l, int32_t* rt)
{
    int32_t hw = (r > 0) ? half_width(r, ys - FRAC * cy) : -1;
    if (hw <= 0)
        return false;
    *l = FRAC * cx - hw;
    *rt = FRAC * cx + hw;
    return true;
}

/* One pixel, `coverage` (0 ... 255) of it in the paint - antialiased edges and glyphs */
void draw_cover(const libdmview_surface_t* s, const paint_t* paint, int32_t x, int32_t y, uint32_t coverage)
{
    uint32_t color = (paint->grad != NULL) ? gradient_color(paint, s->format, x, y) : paint->color;
    uint32_t a = ((color >> 24) * coverage + 127u) / 255u;
    if (a == 0)
        return;
    color = (color & 0x00FFFFFFu) | (a << 24);
    uint8_t* row = (uint8_t*)s->pixels + (uint32_t)y * s->stride;
    if (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        uint16_t* p = (uint16_t*)row + x;
        *p = (a == 255u) ? to_rgb565(color) : blend565(*p, color);
    }
    else
    {
        uint32_t* p = (uint32_t*)row + x;
        *p = (a == 255u) ? color : blend8888(*p, color);
    }
}

/* Pixels from..to of row y that an edge crosses */
static void fringe(const libdmview_surface_t* s, const rect_t* clip, int32_t y, int32_t from, int32_t to,
                   const int32_t* l, const int32_t* r, const paint_t* paint)
{
    if (from < clip->x0)
        from = clip->x0;
    if (to > clip->x1)
        to = clip->x1;
    for (int32_t x = from; x < to; x++)
    {
        int32_t p0 = FRAC * x, p1 = p0 + FRAC, cover = 0;
        for (int32_t k = 0; k < SUB; k++)
        {
            int32_t a = (l[k] > p0) ? l[k] : p0, b = (r[k] < p1) ? r[k] : p1;
            if (b > a)
                cover += b - a;
        }
        if (cover > 0)
            draw_cover(s, paint, x, y, (cover >= FULL) ? 255u : ((uint32_t)cover * 255u + FULL / 2u) / FULL);
    }
}

/* One segment of row y: what every sub-row covers is a span, the rest fringe */
static void aa_segment(const libdmview_surface_t* s, const rect_t* clip, int32_t y, const int32_t* l, const int32_t* r,
                       const paint_t* paint)
{
    int32_t lmin = INT32_MAX, lmax = INT32_MIN, rmin = INT32_MAX, rmax = INT32_MIN;
    bool every = true;
    for (int32_t k = 0; k < SUB; k++)
    {
        if (l[k] >= r[k])
        {
            every = false;
            continue;
        }
        if (l[k] < lmin) lmin = l[k];
        if (l[k] > lmax) lmax = l[k];
        if (r[k] < rmin) rmin = r[k];
        if (r[k] > rmax) rmax = r[k];
    }
    if (lmin == INT32_MAX)
        return;

    int32_t x0 = lmin >> FRAC_SHIFT, x1 = (rmax + FRAC - 1) >> FRAC_SHIFT;
    int32_t i0 = (lmax + FRAC - 1) >> FRAC_SHIFT, i1 = rmin >> FRAC_SHIFT;
    if (!every || i1 <= i0)
        i0 = i1 = x1;                                               /* No inside: all of it fringe */
    if (i1 > i0)
        clipped_span(s, clip, y, i0, i1, paint);
    fringe(s, clip, y, x0, i0, l, r, paint);
    fringe(s, clip, y, i1, x1, l, r, paint);
}

void draw_rrect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                const paint_t* paint)
{
    r = clamp_radius(r, w, h);
    if (r > MAX_RADIUS)
        r = MAX_RADIUS;
    if (r == 0)
    {
        fill(s, clip, x, y, x + w, y + h, paint);
        return;
    }
    int32_t y0 = (y > clip->y0) ? y : clip->y0, y1 = (y + h < clip->y1) ? y + h : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t l[SUB], rt[SUB];
        for (int32_t k = 0; k < SUB; k++)
        {
            if (!rrect_edges(x, y, w, h, r, sub_y(row, k), &l[k], &rt[k]))
                l[k] = rt[k] = 0;
        }
        aa_segment(s, clip, row, l, rt, paint);
    }
}

/* A row of an outline: left and right of the inner shape - or, where a
 * sub-row misses the inner shape, the outer one split at `split` (a pixel
 * boundary, so the halves meet without a seam). */
static void outline_row(const libdmview_surface_t* s, const rect_t* clip, int32_t row, const int32_t* ol,
                        const int32_t* orr, const int32_t* il, const int32_t* ir, const bool* inner, int32_t split,
                        const paint_t* paint)
{
    int32_t ll[SUB], lr[SUB], rl[SUB], rr[SUB];
    for (int32_t k = 0; k < SUB; k++)
    {
        ll[k] = ol[k];
        lr[k] = inner[k] ? il[k] : split;
        rl[k] = inner[k] ? ir[k] : split;
        rr[k] = orr[k];
        if (lr[k] > orr[k])
            lr[k] = orr[k];
        if (rl[k] < ol[k])
            rl[k] = ol[k];
    }
    aa_segment(s, clip, row, ll, lr, paint);
    aa_segment(s, clip, row, rl, rr, paint);
}

void draw_rframe(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                 int32_t t, const paint_t* paint)
{
    if (t <= 0)
        return;
    r = clamp_radius(r, w, h);
    if (r > MAX_RADIUS)
        r = MAX_RADIUS;
    int32_t ix = x + t, iy = y + t, iw = w - 2 * t, ih = h - 2 * t, ir = (r > t) ? r - t : 0;
    int32_t split = FRAC * (x + w / 2);
    int32_t y0 = (y > clip->y0) ? y : clip->y0, y1 = (y + h < clip->y1) ? y + h : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t ol[SUB], orr[SUB], il[SUB], irr[SUB];
        bool inner[SUB];
        for (int32_t k = 0; k < SUB; k++)
        {
            int32_t ys = sub_y(row, k);
            if (!rrect_edges(x, y, w, h, r, ys, &ol[k], &orr[k]))
                ol[k] = orr[k] = 0;
            inner[k] = iw > 0 && ih > 0 && rrect_edges(ix, iy, iw, ih, ir, ys, &il[k], &irr[k]);
        }
        outline_row(s, clip, row, ol, orr, il, irr, inner, split, paint);
    }
}

void draw_circle(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, const paint_t* paint)
{
    if (r > MAX_RADIUS)
        r = MAX_RADIUS;
    int32_t y0 = (cy - r > clip->y0) ? cy - r : clip->y0, y1 = (cy + r < clip->y1) ? cy + r : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t l[SUB], rt[SUB];
        for (int32_t k = 0; k < SUB; k++)
        {
            if (!circle_edges(cx, cy, r, sub_y(row, k), &l[k], &rt[k]))
                l[k] = rt[k] = 0;
        }
        aa_segment(s, clip, row, l, rt, paint);
    }
}

void draw_ring(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, int32_t t, const paint_t* paint)
{
    if (t <= 0)
        return;
    if (r > MAX_RADIUS)
        r = MAX_RADIUS;
    int32_t ir = r - t;
    int32_t y0 = (cy - r > clip->y0) ? cy - r : clip->y0, y1 = (cy + r < clip->y1) ? cy + r : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t ol[SUB], orr[SUB], il[SUB], irr[SUB];
        bool inner[SUB];
        for (int32_t k = 0; k < SUB; k++)
        {
            int32_t ys = sub_y(row, k);
            if (!circle_edges(cx, cy, r, ys, &ol[k], &orr[k]))
                ol[k] = orr[k] = 0;
            inner[k] = ir > 0 && circle_edges(cx, cy, ir, ys, &il[k], &irr[k]);
        }
        outline_row(s, clip, row, ol, orr, il, irr, inner, FRAC * cx, paint);
    }
}

void draw_line(const libdmview_surface_t* s, const rect_t* clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t t,
               const paint_t* paint)
{
    if (t <= 0)
        return;
    int32_t lo = t / 2, hi = t - t / 2;

    /* Straight lines are rectangles */
    if (y0 == y1)
    {
        fill(s, clip, (x0 < x1) ? x0 : x1, y0 - lo, ((x0 < x1) ? x1 : x0) + 1, y0 + hi, paint);
        return;
    }
    if (x0 == x1)
    {
        fill(s, clip, x0 - lo, (y0 < y1) ? y0 : y1, x0 + hi, ((y0 < y1) ? y1 : y0) + 1, paint);
        return;
    }

    /* Bresenham with a t x t brush */
    int32_t dx = (x1 > x0) ? x1 - x0 : x0 - x1, sx = (x0 < x1) ? 1 : -1;
    int32_t dy = (y1 > y0) ? y0 - y1 : y1 - y0, sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;
    for (;;)
    {
        fill(s, clip, x0 - lo, y0 - lo, x0 + hi, y0 + hi, paint);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}
