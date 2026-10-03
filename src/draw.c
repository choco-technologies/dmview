#include "private.h"

/*
 * Rasterizer. Everything is drawn as horizontal spans, clipped to a
 * rectangle; curves use an integer square root per line, no floating point.
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
        for (; n > 0; n--, p++)
            *p = blend565(*p, color);
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

/* Half width (pixels) of a circle of radius r on the line whose center is
 * `d2` half-pixels from the circle's center; -1 when the line misses it. */
static inline int32_t circle_half_width(int32_t r, int32_t d2)
{
    int32_t r2 = 2 * r;
    if (d2 < 0)
        d2 = -d2;
    if (d2 >= r2)
        return -1;
    return (int32_t)(isqrt((uint32_t)(r2 * r2 - d2 * d2)) / 2u);
}

/* Horizontal extent of line `row` of a rounded rectangle; false outside it. */
static bool rrect_extent(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t row, int32_t* x0, int32_t* x1)
{
    if (row < y || row >= y + h || w <= 0)
        return false;
    int32_t inset = 0;
    if (row < y + r || row >= y + h - r)
    {
        /* Center of the corner circle, in half-pixels from this line's center */
        int32_t cy = (row < y + r) ? y + r : y + h - r;
        int32_t hw = circle_half_width(r, 2 * cy - (2 * row + 1));
        inset = (hw < 0) ? r : r - hw;
    }
    *x0 = x + inset;
    *x1 = x + w - inset;
    return *x1 > *x0;
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

void draw_rrect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                const paint_t* paint)
{
    r = clamp_radius(r, w, h);
    if (r == 0)
    {
        fill(s, clip, x, y, x + w, y + h, paint);
        return;
    }
    int32_t y0 = (y > clip->y0) ? y : clip->y0, y1 = (y + h < clip->y1) ? y + h : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t a, b;
        if (rrect_extent(x, y, w, h, r, row, &a, &b))
            clipped_span(s, clip, row, a, b, paint);
    }
}

void draw_rframe(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                 int32_t t, const paint_t* paint)
{
    if (t <= 0)
        return;
    r = clamp_radius(r, w, h);
    int32_t ix = x + t, iy = y + t, iw = w - 2 * t, ih = h - 2 * t, ir = (r > t) ? r - t : 0;
    int32_t y0 = (y > clip->y0) ? y : clip->y0, y1 = (y + h < clip->y1) ? y + h : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t a, b, ia, ib;
        if (!rrect_extent(x, y, w, h, r, row, &a, &b))
            continue;
        if (iw > 0 && ih > 0 && rrect_extent(ix, iy, iw, ih, ir, row, &ia, &ib))
        {
            clipped_span(s, clip, row, a, ia, paint);
            clipped_span(s, clip, row, ib, b, paint);
        }
        else
            clipped_span(s, clip, row, a, b, paint);
    }
}

void draw_circle(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, const paint_t* paint)
{
    int32_t y0 = (cy - r > clip->y0) ? cy - r : clip->y0, y1 = (cy + r < clip->y1) ? cy + r : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t hw = circle_half_width(r, 2 * cy - (2 * row + 1));
        if (hw > 0)
            clipped_span(s, clip, row, cx - hw, cx + hw, paint);
    }
}

void draw_ring(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, int32_t t, const paint_t* paint)
{
    int32_t ir = r - t;
    if (t <= 0)
        return;
    int32_t y0 = (cy - r > clip->y0) ? cy - r : clip->y0, y1 = (cy + r < clip->y1) ? cy + r : clip->y1;
    for (int32_t row = y0; row < y1; row++)
    {
        int32_t d2 = 2 * cy - (2 * row + 1);
        int32_t hw = circle_half_width(r, d2);
        if (hw <= 0)
            continue;
        int32_t ihw = (ir > 0) ? circle_half_width(ir, d2) : -1;
        if (ihw > 0)
        {
            clipped_span(s, clip, row, cx - hw, cx - ihw, paint);
            clipped_span(s, clip, row, cx + ihw, cx + hw, paint);
        }
        else
            clipped_span(s, clip, row, cx - hw, cx + hw, paint);
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
