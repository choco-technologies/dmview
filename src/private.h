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
    uint8_t*        font_scale;     /* Built-in font magnification per font */
    uint32_t        box_count;
    rbox_t*         boxes;
    uint32_t        item_count;
    item_t*         items;

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
};

/* validate.c */
dmv_status_t dmv_validate(const dmv_input_t* input, uint32_t* error_offset);

/* claims.c */
int         claims_init(void);
void        claims_deinit(void);

/* view.c */
const char* view_string(const struct libdmview* v, uint32_t index);
void        view_set_int(struct libdmview* v, uint32_t index, int32_t value);
void        view_set_string(struct libdmview* v, uint32_t index, const char* value);
void        view_invalidate_deps(struct libdmview* v, uint32_t word, uint32_t bit);
void        view_mark_dirty(struct libdmview* v, int32_t box);

/* exec.c */
void        exec_handler(struct libdmview* v, int32_t box, uint16_t label);
void        exec_draw(struct libdmview* v, rect_t* changed);

/* draw.c - everything clipped to `clip`, colors 0xAARRGGBB */
bool        draw_supported(uint8_t format);
void        draw_rect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void        draw_frame(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, uint32_t color);
void        draw_rrect(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void        draw_rframe(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t t, uint32_t color);
void        draw_circle(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, uint32_t color);
void        draw_ring(const libdmview_surface_t* s, const rect_t* clip, int32_t cx, int32_t cy, int32_t r, int32_t t, uint32_t color);
void        draw_line(const libdmview_surface_t* s, const rect_t* clip, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t t, uint32_t color);

/* font.c */
uint8_t     font_scale_for(const char* spec);
void        draw_text(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
                      const char* text, uint8_t scale, uint32_t color, uint8_t align);

#endif /* LIBDMVIEW_PRIVATE_H */
