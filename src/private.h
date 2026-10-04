#ifndef LIBDMVIEW_PRIVATE_H
#define LIBDMVIEW_PRIVATE_H

#include "dmod.h"
#include "libdmview.h"

#define VIEW_MAGIC          0x444D5657u     /* 'DMVW' */
#define MAX_BOX_DEPTH       32u
#define MAX_CALL_DEPTH      8u
#define MAX_STEPS           1000000u        /* Instructions per run - stops a runaway loop */
#define ROOT                (-1)

/* Runtime flags of a box, next to the DMV_BOX_* bits from the file */
#define BOXF_VISIBLE        0x10u           /* Reached by the last draw */
#define BOXF_DIRTY          0x20u           /* Has to be redrawn */
#define BOXF_GEOMETRY_VAR   0x40u           /* Position or size comes from a variable */
#define BOXF_TRANSLUCENT    0x80u           /* It or a box around it has an OPACITY: never opaque */

/* Dependency bits of the built-in word (after the variable words) */
#define DEP_PRESSED         0x01u
#define DEP_SCROLL          0x02u
#define DEP_FOCUSED         0x04u
#define DEP_CONTACTS        0x08u

/* Little-endian access; the code is 4-byte aligned in memory, operands at
 * their natural alignment, so these compile to plain loads on LE cores. */
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Half-open rectangle in surface coordinates. */
typedef struct
{
    int32_t x0, y0, x1, y1;
} rect_t;

static inline bool rect_empty(const rect_t* r) { return r->x1 <= r->x0 || r->y1 <= r->y0; }

static inline rect_t rect_and(rect_t a, const rect_t* b)
{
    if (b->x0 > a.x0) a.x0 = b->x0;
    if (b->y0 > a.y0) a.y0 = b->y0;
    if (b->x1 < a.x1) a.x1 = b->x1;
    if (b->y1 < a.y1) a.y1 = b->y1;
    return a;
}

static inline void rect_add(rect_t* to, const rect_t* r)
{
    if (rect_empty(r))
        return;
    if (rect_empty(to))
    {
        *to = *r;
        return;
    }
    if (r->x0 < to->x0) to->x0 = r->x0;
    if (r->y0 < to->y0) to->y0 = r->y0;
    if (r->x1 > to->x1) to->x1 = r->x1;
    if (r->y1 > to->y1) to->y1 = r->y1;
}

/* ---- Pixels ---- */

static inline uint16_t to_rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
}

/* (src * a + dst * (255 - a)) / 255 for one channel */
static inline uint32_t mix(uint32_t src, uint32_t dst, uint32_t a)
{
    uint32_t x = src * a + dst * (255u - a) + 128u;
    return (x + (x >> 8)) >> 8;
}

static inline uint16_t blend565(uint16_t d, uint32_t color)
{
    uint32_t a = color >> 24;
    uint32_t dr = ((d >> 11) & 0x1Fu) << 3, dg = ((d >> 5) & 0x3Fu) << 2, db = (d & 0x1Fu) << 3;
    return (uint16_t)(((mix((color >> 16) & 0xFFu, dr, a) & 0xF8u) << 8) |
                      ((mix((color >> 8) & 0xFFu, dg, a) & 0xFCu) << 3) | (mix(color & 0xFFu, db, a) >> 3));
}

static inline uint32_t blend8888(uint32_t d, uint32_t color)
{
    uint32_t a = color >> 24;
    return 0xFF000000u | (mix((color >> 16) & 0xFFu, (d >> 16) & 0xFFu, a) << 16) |
           (mix((color >> 8) & 0xFFu, (d >> 8) & 0xFFu, a) << 8) | mix(color & 0xFFu, d & 0xFFu, a);
}

/* ---- Gradients ---- */

#define GRADIENT_STEPS      256u            /* Palette entries of a gradient */
#define GRADIENT_NO_LUT     0xFFu           /* lut_format: palette not built yet */

/** A gradient of the view, with its palette. */
typedef struct
{
    uint8_t     kind;               /* DMV_GRADIENT_* */
    uint8_t     count;              /* Stops */
    uint16_t    first;              /* First stop in the view's stop table */
    int16_t     param[4];
    bool        opaque;             /* Every stop opaque: written without blending */
    uint8_t     lut_format;         /* Pixel format `lut` was built for, GRADIENT_NO_LUT */
    uint32_t    lut[GRADIENT_STEPS];    /* 0xAARRGGBB */
} grad_t;

/**
 * What a shape is drawn with: a color, or a gradient placed on the shape's
 * rectangle. Gradient positions are 16.16 fixed point, x and y relative to
 * the shape's origin (ox, oy):
 *  - linear: palette index = a0 + x * ax + y * ay;
 *  - radial: u = a0 + x * ax, v = b0 + y * by (1.0 = the radius),
 *    palette index = sqrt(u^2 + v^2) * 256.
 */
typedef struct
{
    uint32_t        color;          /* 0xAARRGGBB, when grad is NULL - its alpha includes `alpha` */
    const grad_t*   grad;
    uint32_t        alpha;          /* Opacity of the box drawn in, 0 ... 255 */
    int32_t         ox, oy;         /* Origin of the shape the gradient is placed on */
    int32_t         a0, ax, ay;     /* At the middle of the pixel at ox, oy */
    int32_t         b0, by;
} paint_t;

/* ---- Fonts ---- */

#define FONT_ASCII_FIRST    0x20u
#define FONT_ASCII_COUNT    95u             /* U+0020 ... U+007E */
#define FONT_NO_GLYPH       0xFFFFu

/** A font file (.dmvf), loaded once and shared by every view that uses it. */
typedef struct font_file
{
    struct font_file*   next;
    char*               path;
    uint32_t            refs;
    uint8_t*            data;               /* The whole file */
    const uint8_t*      glyphs;             /* dmvf_glyph_t records */
    const uint8_t*      bitmaps;
    uint32_t            count;
    uint16_t            line_height;
    int16_t             ascent;
    uint16_t            fallback;           /* Glyph of '?', FONT_NO_GLYPH */
    uint16_t            ascii[FONT_ASCII_COUNT];    /* Glyph of each ASCII character, FONT_NO_GLYPH */
} font_file_t;

/** A font of a view: a font file, or the built-in 8x8 font magnified. */
typedef struct
{
    font_file_t*        file;               /* NULL: the built-in font */
    uint8_t             scale;              /* Magnification of the built-in font */
} font_t;

/* ---- Images ---- */

/** An image file (.dmvi), loaded once and shared by every view that shows it. */
typedef struct image
{
    struct image*       next;
    char*               path;
    uint32_t            refs;
    bool                stale;              /* RELOAD: no longer found, freed by its last user */
    bool                opaque;             /* Every pixel opaque: copied without blending */
    uint8_t             format;             /* dmvi_format_t */
    uint16_t            width, height;
    uint32_t            stride, alpha_stride;
    uint8_t*            data;               /* The whole file */
    const uint8_t*      pixels;
    const uint8_t*      alpha;              /* RGB565A8 */
    uint32_t*           palette;            /* I8: DMVI_PALETTE_MAX colors, the missing ones transparent */
} image_t;

/** What one IMAGE / ICON instruction of a view shows. */
typedef struct
{
    uint32_t            pc;                 /* Byte offset of the instruction */
    char*               source;             /* Its path operand as last drawn, NULL before */
    image_t*            image;              /* NULL: not loadable */
    int32_t             box;                /* Box it was last drawn in */
} image_slot_t;

/** A box at run time. Geometry is what its last draw found. */
typedef struct
{
    uint16_t    begin;              /* Code word offset of BOX */
    uint16_t    end;                /* ... of END */
    int16_t     parent;             /* Box index, ROOT */
    uint8_t     flags;              /* DMV_BOX_* | BOXF_* */
    uint8_t     scroll_flags;       /* DMV_SCROLL_* when the box scrolls */
    rect_t      bounds;             /* Absolute */
    rect_t      clip;               /* What of it is on the screen: bounds and the parent's clip */
    int32_t     parent_ox;          /* Origin of the parent's content */
    int32_t     parent_oy;
    rect_t      parent_clip;        /* What of the parent is on the screen */
    int32_t     sx, sy;             /* Scroll offset */
    int32_t     cw, ch;             /* Scrolled content size */
    uint16_t    handlers[DMV_EVENT_COUNT];  /* Label word offsets, DMV_NONE */
} rbox_t;

/** One level of box nesting while executing. */
typedef struct
{
    int32_t     ox, oy;             /* Origin of the content */
    rect_t      clip;               /* Where drawing goes - also limited to the area being redrawn */
    rect_t      view_clip;          /* What of the box is on the screen at all */
    int32_t     box;                /* Box index, ROOT */
    uint32_t    alpha;              /* Opacity: the boxes' OPACITY multiplied, 255 */
} frame_t;

typedef struct
{
    uint8_t     kind;
    uint8_t     arg;
    uint16_t    label;
    uint32_t    value;
    uint32_t    due;                /* .timer: next run, in view time */
} item_t;

struct libdmview
{
    uint32_t        magic;

    /* From the file */
    uint8_t*        code;           /* Instructions */
    uint32_t        code_size;
    uint8_t*        strings;        /* String table: offsets, then the texts */
    uint32_t        string_count;
    uint16_t        width, height;
    uint16_t        entry;
    uint16_t        longpress_ms;
    uint32_t        var_count;
    dmv_var_t*      vars;
    uint32_t        font_count;
    font_t*         fonts;
    uint32_t        box_count;
    rbox_t*         boxes;
    uint32_t        item_count;
    item_t*         items;
    uint32_t        gradient_count;
    grad_t*         gradients;
    dmv_stop_t*     stops;

    /* Variables */
    int32_t*        ints;           /* Integer values (string variables unused) */
    char**          strs;           /* Buffer of each string variable, NULL for integers */

    /* Dependencies: per box (and the root, last row) the variables it read
     * while drawing - var_count bits, then one word of DEP_* bits. */
    uint32_t        dep_words;
    uint32_t*       deps;

    /* Drawing */
    bool            full;           /* Everything has to be drawn */
    bool            any_dirty;
    const libdmview_surface_t* surface;
    uint16_t        surface_w, surface_h;

    /* Execution */
    bool            drawing;        /* A draw pass - else a handler */
    int32_t         handler_box;    /* Box context of a handler */
    frame_t         frames[MAX_BOX_DEPTH + 1];
    uint32_t        depth;
    int32_t         ev[DMV_VAR_EV_KEY - DMV_VAR_EV_CONTACT + 1];

    /* Input */
    int32_t         captured;       /* Box holding the contact, ROOT for none */
    bool            down;
    bool            long_fired;
    int32_t         last_x, last_y;
    uint32_t        press_ms;
    uint32_t        buttons;
    bool            have_now;
    uint32_t        start_ms, now_ms;

    char*           goto_path;
    char*           goto_taken;

    /* Images */
    char*           dir;            /* Directory of the view file - relative image paths start there */
    uint32_t        slot_count;
    image_slot_t*   slots;          /* One per IMAGE / ICON, by pc ascending */
};

/* validate.c */
dmv_status_t dmv_validate(const dmv_input_t* input, uint32_t* error_offset);

/* claims.c */
int         claims_init(void);
void        claims_deinit(void);

/* fontfile.c */
int             fonts_init(void);
void            fonts_deinit(void);
void            font_resolve(const char* spec, font_t* font);
void            font_release(font_t* font);
const uint8_t*  font_glyph(const font_file_t* f, uint32_t codepoint);   /* dmvf_glyph_t, NULL if none */

/* image.c */
int             images_init(void);
void            images_deinit(void);
int             images_slots(struct libdmview* v);         /* Builds v->slots from the code */
void            images_free_slots(struct libdmview* v);
const image_t*  images_get(struct libdmview* v, uint32_t pc, const char* source);   /* NULL: nothing to draw */
void            images_reload(struct libdmview* v, const char* source);
void            draw_image(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
                           const image_t* image, uint32_t alpha, uint8_t align);
void            draw_icon(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
                          const image_t* image, const paint_t* paint, uint8_t align);

/* view.c */
const char* view_string(const struct libdmview* v, uint32_t index);
void        view_set_int(struct libdmview* v, uint32_t index, int32_t value);
void        view_set_string(struct libdmview* v, uint32_t index, const char* value);
void        view_invalidate_deps(struct libdmview* v, uint32_t word, uint32_t bit);
void        view_mark_dirty(struct libdmview* v, int32_t box);

/* exec.c */
void        exec_handler(struct libdmview* v, int32_t box, uint16_t label);
void        exec_draw(struct libdmview* v, rect_t* changed);

/* draw.c - everything clipped to `clip`, drawn with a paint */
bool        draw_supported(uint8_t format);
void        draw_span(const libdmview_surface_t* s, int32_t y, int32_t x0, int32_t x1, uint32_t color);
void        draw_pixels(const libdmview_surface_t* s, int32_t y, int32_t x0, int32_t x1, uint32_t pixel);
void        draw_cover(const libdmview_surface_t* s, const paint_t* paint, int32_t x, int32_t y, uint32_t coverage);   /* 0 ... 255 */
void        draw_rect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, const paint_t* paint);
void        draw_frame(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, const paint_t* paint);
void        draw_rrect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const paint_t* paint);
void        draw_rframe(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t t, const paint_t* paint);
void        draw_circle(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, const paint_t* paint);
void        draw_ring(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, int32_t t, const paint_t* paint);
void        draw_line(const libdmview_surface_t* s, const rect_t* clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t t, const paint_t* paint);

/* gradient.c */
void        paint_gradient(paint_t* paint, grad_t* grad, const dmv_stop_t* stops, uint8_t format,
                           int32_t x, int32_t y, int32_t w, int32_t h);
void        gradient_span(const libdmview_surface_t* s, const paint_t* paint, int32_t y, int32_t x0, int32_t x1);
uint32_t    gradient_color(const paint_t* paint, uint8_t format, int32_t x, int32_t y);

/* font.c */
uint8_t     font_scale_for(const char* spec);
void        draw_text(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
                      const char* text, const font_t* font, const paint_t* paint, uint8_t align);

#endif /* LIBDMVIEW_PRIVATE_H */
