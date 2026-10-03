#include "private.h"
#include "layout.h"
#include "dmhaman.h"
#include <errno.h>
#include <string.h>

/*
 * The interpreter. One switch over the opcodes; every operand is read at a
 * constant offset (OPOFF()). The view was validated when it was loaded, so no
 * index or offset is checked here.
 *
 * While drawing, every variable read is recorded for the innermost box (its
 * dependency row): a later change of the variable redraws exactly the boxes
 * that read it.
 */

#define VARBIT(insn, i)     (((insn)[2] & (1u << (i))) != 0)

/* ---- Variables ---- */

static inline int32_t context_box(const struct libdmview* v)
{
    return v->drawing ? v->frames[v->depth].box : v->handler_box;
}

static inline void record(struct libdmview* v, uint32_t word, uint32_t bit)
{
    if (!v->drawing)
        return;
    int32_t box = v->frames[v->depth].box;
    uint32_t row = (box == ROOT) ? v->box_count : (uint32_t)box;
    v->deps[row * v->dep_words + word] |= bit;
}

static inline void record_builtin(struct libdmview* v, uint32_t bit)
{
    record(v, v->dep_words - 1U, bit);
}

static int32_t builtin_value(struct libdmview* v, uint32_t index)
{
    int32_t box = context_box(v);
    const rbox_t* b = (box != ROOT) ? &v->boxes[box] : NULL;

    switch (index)
    {
        case DMV_VAR_BOX_W:
            return (b != NULL) ? b->bounds.x1 - b->bounds.x0 : (int32_t)v->surface_w;
        case DMV_VAR_BOX_H:
            return (b != NULL) ? b->bounds.y1 - b->bounds.y0 : (int32_t)v->surface_h;
        case DMV_VAR_BOX_PRESSED:
            record_builtin(v, DEP_PRESSED);
            return (box != ROOT && box == v->captured && v->down) ? 1 : 0;
        case DMV_VAR_BOX_CONTACTS:
            record_builtin(v, DEP_CONTACTS);
            return (box != ROOT && box == v->captured && v->down) ? 1 : 0;
        case DMV_VAR_BOX_SX:
            record_builtin(v, DEP_SCROLL);
            return (b != NULL) ? b->sx : 0;
        case DMV_VAR_BOX_SY:
            record_builtin(v, DEP_SCROLL);
            return (b != NULL) ? b->sy : 0;
        case DMV_VAR_BOX_FOCUSED:
            record_builtin(v, DEP_FOCUSED);
            return 0;
        case DMV_VAR_VIEW_W:
            return v->surface_w;
        case DMV_VAR_VIEW_H:
            return v->surface_h;
        case DMV_VAR_TIME:
            return (int32_t)(v->now_ms - v->start_ms);
        default:
            return (index >= DMV_VAR_EV_CONTACT && index <= DMV_VAR_EV_KEY) ? v->ev[index - DMV_VAR_EV_CONTACT] : 0;
    }
}

static inline int32_t read_int(struct libdmview* v, uint32_t index)
{
    if (index >= DMV_BUILTIN_BASE)
        return builtin_value(v, index);
    record(v, index / 32U, 1U << (index % 32U));
    return v->ints[index];
}

static inline int32_t v16(struct libdmview* v, const uint8_t* insn, unsigned i, unsigned off)
{
    return VARBIT(insn, i) ? read_int(v, rd16(insn + off)) : (int32_t)(int16_t)rd16(insn + off);
}

static inline int32_t v32(struct libdmview* v, const uint8_t* insn, unsigned i, unsigned off)
{
    return VARBIT(insn, i) ? read_int(v, rd16(insn + off)) : (int32_t)rd32(insn + off);
}

static inline const char* vstr(struct libdmview* v, const uint8_t* insn, unsigned i, unsigned off)
{
    uint16_t index = rd16(insn + off);
    if (!VARBIT(insn, i))
        return view_string(v, index);
    record(v, index / 32U, 1U << (index % 32U));
    return v->strs[index];
}

/* The paint of a drawing instruction's color operand: its color, or the
 * gradient (DMV_PAINT_GRADIENT) placed on the shape's rectangle x, y, w, h */
static inline void paint_of(struct libdmview* v, const uint8_t* insn, unsigned i, unsigned off, paint_t* paint,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{
    if ((insn[3] & DMV_PAINT_GRADIENT) == 0)
    {
        paint->color = (uint32_t)v32(v, insn, i, off);
        paint->grad = NULL;
        return;
    }
    paint_gradient(paint, &v->gradients[rd32(insn + off)], v->stops, v->surface->format, x, y, w, h);
}

/* "Count: %d" with one %d / %x, "%%" for '%' (validated by the assembler) */
static void format_into(char* out, size_t size, const char* format, int32_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t o = 0;
    for (const char* p = format; *p != '\0' && o + 1 < size; p++)
    {
        if (*p != '%' || (p[1] != 'd' && p[1] != 'x' && p[1] != '%'))
        {
            out[o++] = *p;
            continue;
        }
        char conversion = *++p;
        if (conversion == '%')
        {
            out[o++] = '%';
            continue;
        }
        char tmp[12];
        size_t t = 0;
        uint32_t base = (conversion == 'x') ? 16u : 10u;
        uint32_t u = (conversion == 'd' && n < 0) ? (uint32_t)(-(n + 1)) + 1u : (uint32_t)n;
        do
        {
            tmp[t++] = digits[u % base];
            u /= base;
        } while (u != 0);
        if (conversion == 'd' && n < 0 && o + 1 < size)
            out[o++] = '-';
        while (t > 0 && o + 1 < size)
            out[o++] = tmp[--t];
    }
    out[o] = '\0';
}

/* ---- Actions ---- */

/* "cmd arg arg" -> a detached module run, split at spaces */
static void run_command(const char* line)
{
    char buffer[128];
    char* argv[16];
    int argc = 0;
    size_t len = strlen(line);
    if (len >= sizeof(buffer))
        len = sizeof(buffer) - 1U;
    memcpy(buffer, line, len);
    buffer[len] = '\0';
    for (char* p = buffer; *p != '\0' && argc < (int)(sizeof(argv) / sizeof(argv[0])); )
    {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ')
            p++;
    }
    if (argc > 0)
        (void)Dmod_RunModuleDetached(argv[0], argc, argv, NULL);
}

static void set_goto(struct libdmview* v, const char* path)
{
    char* copy = Dmod_StrDup(path);
    if (copy == NULL)
        return;
    if (v->goto_path != NULL)
        Dmod_Free(v->goto_path);
    v->goto_path = copy;
}

static void scroll_to(struct libdmview* v, uint16_t box, int32_t x, int32_t y)
{
    rbox_t* b = &v->boxes[box];
    int32_t max_x = b->cw - (b->bounds.x1 - b->bounds.x0), max_y = b->ch - (b->bounds.y1 - b->bounds.y0);
    x = (x > max_x) ? max_x : x;
    y = (y > max_y) ? max_y : y;
    x = (x < 0) ? 0 : x;
    y = (y < 0) ? 0 : y;
    if (x == b->sx && y == b->sy)
        return;
    b->sx = x;
    b->sy = y;
    view_mark_dirty(v, (int32_t)box);
    view_invalidate_deps(v, v->dep_words - 1U, DEP_SCROLL);
}

/* ---- Boxes ---- */

static void enter_box(struct libdmview* v, const uint8_t* insn)
{
    uint16_t index = rd16(insn + OPOFF(BOX, 0));
    rbox_t* b = &v->boxes[index];
    frame_t* parent = &v->frames[v->depth];

    /* Geometry is read in the parent's context - a variable in it makes
     * the parent depend on it */
    int32_t x = parent->ox + v16(v, insn, 1, OPOFF(BOX, 1));
    int32_t y = parent->oy + v16(v, insn, 2, OPOFF(BOX, 2));
    int32_t w = v16(v, insn, 3, OPOFF(BOX, 3));
    int32_t h = v16(v, insn, 4, OPOFF(BOX, 4));

    b->bounds.x0 = x;
    b->bounds.y0 = y;
    b->bounds.x1 = x + ((w > 0) ? w : 0);
    b->bounds.y1 = y + ((h > 0) ? h : 0);
    /* What is on the screen does not depend on the area being redrawn */
    b->clip = rect_and(b->bounds, &parent->view_clip);
    b->parent_ox = parent->ox;
    b->parent_oy = parent->oy;
    b->parent_clip = parent->view_clip;
    b->flags = (uint8_t)((b->flags | BOXF_VISIBLE) & ~BOXF_DIRTY);
    memset(v->deps + index * v->dep_words, 0, v->dep_words * sizeof(uint32_t));

    frame_t* f = &v->frames[++v->depth];
    f->ox = x;
    f->oy = y;
    f->clip = rect_and(b->bounds, &parent->clip);
    f->view_clip = b->clip;
    f->box = index;
}

/* ---- The interpreter ---- */

static void run(struct libdmview* v, uint32_t pc, bool box_region)
{
    uint16_t calls[MAX_CALL_DEPTH];
    uint32_t call_depth = 0;
    const libdmview_surface_t* s = v->surface;
    paint_t paint;

    for (uint32_t steps = MAX_STEPS; steps != 0; steps--)
    {
        const uint8_t* insn = v->code + pc;
        uint32_t next = pc + insn[1];
        frame_t* f = &v->frames[v->depth];
        bool draw = v->drawing && !rect_empty(&f->clip);

        switch (insn[0])
        {
            case DMV_OP_NOP:
                break;

            /* ---- Structure and flow ---- */
            case DMV_OP_BOX:
                if (v->drawing)
                    enter_box(v, insn);
                break;
            case DMV_OP_END:
                if (v->drawing && v->depth > 0)
                {
                    v->depth--;
                    if (box_region && v->depth == 0)
                        return;
                }
                break;
            case DMV_OP_JMP:
                next = rd16(insn + OPOFF(JMP, 0)) * DMV_CODE_WORD;
                break;
            case DMV_OP_CALL:
                if (call_depth == MAX_CALL_DEPTH)
                {
                    DMOD_LOG_ERROR("libdmview: calls nested too deep\n");
                    return;
                }
                calls[call_depth++] = (uint16_t)(next / DMV_CODE_WORD);
                next = rd16(insn + OPOFF(CALL, 0)) * DMV_CODE_WORD;
                break;
            case DMV_OP_RET:
                if (call_depth == 0)
                    return;
                next = calls[--call_depth] * DMV_CODE_WORD;
                break;
            case DMV_OP_JEQ:
            case DMV_OP_JNE:
            case DMV_OP_JLT:
            case DMV_OP_JLE:
            case DMV_OP_JGT:
            case DMV_OP_JGE:
            {
                int32_t a = v32(v, insn, 0, OPOFF(JEQ, 0)), b = v32(v, insn, 1, OPOFF(JEQ, 1));
                bool jump;
                switch (insn[0])
                {
                    case DMV_OP_JEQ: jump = a == b; break;
                    case DMV_OP_JNE: jump = a != b; break;
                    case DMV_OP_JLT: jump = a < b; break;
                    case DMV_OP_JLE: jump = a <= b; break;
                    case DMV_OP_JGT: jump = a > b; break;
                    default:         jump = a >= b; break;
                }
                if (jump)
                    next = rd16(insn + OPOFF(JEQ, 2)) * DMV_CODE_WORD;
                break;
            }
            case DMV_OP_SCROLL:
                if (v->drawing && f->box != ROOT)
                {
                    rbox_t* b = &v->boxes[f->box];
                    b->cw = v16(v, insn, 0, OPOFF(SCROLL, 0));
                    b->ch = v16(v, insn, 1, OPOFF(SCROLL, 1));
                    b->scroll_flags = insn[3];
                    f->ox -= b->sx;
                    f->oy -= b->sy;
                }
                break;
            case DMV_OP_FOCUS:
                break;

            /* ---- Drawing ---- */
            case DMV_OP_FILL:
                if (draw)
                {
                    /* A gradient spans the whole box, whatever part of it is redrawn */
                    rect_t area = (f->box != ROOT) ? v->boxes[f->box].bounds
                                                   : (rect_t){ 0, 0, v->surface_w, v->surface_h };
                    paint_of(v, insn, 0, OPOFF(FILL, 0), &paint, area.x0, area.y0, area.x1 - area.x0, area.y1 - area.y0);
                    draw_rect(s, &f->clip, f->clip.x0, f->clip.y0, f->clip.x1 - f->clip.x0, f->clip.y1 - f->clip.y0, &paint);
                }
                break;
            case DMV_OP_RECT:
                if (draw)
                {
                    int32_t x = f->ox + v16(v, insn, 0, OPOFF(RECT, 0)), y = f->oy + v16(v, insn, 1, OPOFF(RECT, 1));
                    int32_t w = v16(v, insn, 2, OPOFF(RECT, 2)), h = v16(v, insn, 3, OPOFF(RECT, 3));
                    paint_of(v, insn, 4, OPOFF(RECT, 4), &paint, x, y, w, h);
                    draw_rect(s, &f->clip, x, y, w, h, &paint);
                }
                break;
            case DMV_OP_RRECT:
                if (draw)
                {
                    int32_t x = f->ox + v16(v, insn, 0, OPOFF(RRECT, 0)), y = f->oy + v16(v, insn, 1, OPOFF(RRECT, 1));
                    int32_t w = v16(v, insn, 2, OPOFF(RRECT, 2)), h = v16(v, insn, 3, OPOFF(RRECT, 3));
                    int32_t r = v16(v, insn, 4, OPOFF(RRECT, 4));
                    paint_of(v, insn, 5, OPOFF(RRECT, 5), &paint, x, y, w, h);
                    draw_rrect(s, &f->clip, x, y, w, h, r, &paint);
                }
                break;
            case DMV_OP_FRAME:
                if (draw)
                {
                    int32_t x = f->ox + v16(v, insn, 0, OPOFF(FRAME, 0)), y = f->oy + v16(v, insn, 1, OPOFF(FRAME, 1));
                    int32_t w = v16(v, insn, 2, OPOFF(FRAME, 2)), h = v16(v, insn, 3, OPOFF(FRAME, 3));
                    int32_t t = v16(v, insn, 4, OPOFF(FRAME, 4));
                    paint_of(v, insn, 5, OPOFF(FRAME, 5), &paint, x, y, w, h);
                    draw_frame(s, &f->clip, x, y, w, h, t, &paint);
                }
                break;
            case DMV_OP_RFRAME:
                if (draw)
                {
                    int32_t x = f->ox + v16(v, insn, 0, OPOFF(RFRAME, 0)), y = f->oy + v16(v, insn, 1, OPOFF(RFRAME, 1));
                    int32_t w = v16(v, insn, 2, OPOFF(RFRAME, 2)), h = v16(v, insn, 3, OPOFF(RFRAME, 3));
                    int32_t r = v16(v, insn, 4, OPOFF(RFRAME, 4)), t = v16(v, insn, 5, OPOFF(RFRAME, 5));
                    paint_of(v, insn, 6, OPOFF(RFRAME, 6), &paint, x, y, w, h);
                    draw_rframe(s, &f->clip, x, y, w, h, r, t, &paint);
                }
                break;
            case DMV_OP_LINE:
                if (draw)
                {
                    int32_t x0 = f->ox + v16(v, insn, 0, OPOFF(LINE, 0)), y0 = f->oy + v16(v, insn, 1, OPOFF(LINE, 1));
                    int32_t x1 = f->ox + v16(v, insn, 2, OPOFF(LINE, 2)), y1 = f->oy + v16(v, insn, 3, OPOFF(LINE, 3));
                    int32_t t = v16(v, insn, 4, OPOFF(LINE, 4));
                    /* A gradient spans the line's bounding box, brush included */
                    int32_t lx = ((x0 < x1) ? x0 : x1) - t / 2, ly = ((y0 < y1) ? y0 : y1) - t / 2;
                    paint_of(v, insn, 5, OPOFF(LINE, 5), &paint, lx, ly,
                             ((x0 < x1) ? x1 - x0 : x0 - x1) + t + 1, ((y0 < y1) ? y1 - y0 : y0 - y1) + t + 1);
                    draw_line(s, &f->clip, x0, y0, x1, y1, t, &paint);
                }
                break;
            case DMV_OP_CIRCLE:
                if (draw)
                {
                    int32_t cx = f->ox + v16(v, insn, 0, OPOFF(CIRCLE, 0)), cy = f->oy + v16(v, insn, 1, OPOFF(CIRCLE, 1));
                    int32_t r = v16(v, insn, 2, OPOFF(CIRCLE, 2));
                    paint_of(v, insn, 3, OPOFF(CIRCLE, 3), &paint, cx - r, cy - r, 2 * r, 2 * r);
                    draw_circle(s, &f->clip, cx, cy, r, &paint);
                }
                break;
            case DMV_OP_RING:
                if (draw)
                {
                    int32_t cx = f->ox + v16(v, insn, 0, OPOFF(RING, 0)), cy = f->oy + v16(v, insn, 1, OPOFF(RING, 1));
                    int32_t r = v16(v, insn, 2, OPOFF(RING, 2)), t = v16(v, insn, 3, OPOFF(RING, 3));
                    paint_of(v, insn, 4, OPOFF(RING, 4), &paint, cx - r, cy - r, 2 * r, 2 * r);
                    draw_ring(s, &f->clip, cx, cy, r, t, &paint);
                }
                break;
            case DMV_OP_TEXT:
                if (draw)
                {
                    int32_t x = f->ox + v16(v, insn, 0, OPOFF(TEXT, 0)), y = f->oy + v16(v, insn, 1, OPOFF(TEXT, 1));
                    int32_t w = v16(v, insn, 2, OPOFF(TEXT, 2)), h = v16(v, insn, 3, OPOFF(TEXT, 3));
                    const char* text = vstr(v, insn, 4, OPOFF(TEXT, 4));
                    paint_of(v, insn, 6, OPOFF(TEXT, 6), &paint, x, y, w, h);
                    draw_text(s, &f->clip, x, y, w, h, text, &v->fonts[rd16(insn + OPOFF(TEXT, 5))], &paint,
                              insn[3] & DMV_ALIGN_FLAGS_MASK);
                }
                break;
            case DMV_OP_IMAGE:
                /* Not supported yet - nothing is drawn in its place */
                break;

            /* ---- Variables ---- */
            case DMV_OP_SET:
            {
                uint16_t d = rd16(insn + OPOFF(SET, 0));
                if (v->vars[d].type == DMV_VAR_STR)
                {
                    uint16_t src = rd16(insn + OPOFF(SET, 1));
                    if (VARBIT(insn, 1))
                    {
                        record(v, src / 32U, 1U << (src % 32U));
                        if (src != d)
                            view_set_string(v, d, v->strs[src]);
                    }
                    else
                        view_set_string(v, d, view_string(v, rd32(insn + OPOFF(SET, 1))));
                }
                else
                    view_set_int(v, d, v32(v, insn, 1, OPOFF(SET, 1)));
                break;
            }
            case DMV_OP_ADD:
            case DMV_OP_SUB:
            case DMV_OP_MUL:
            case DMV_OP_DIV:
            case DMV_OP_MOD:
            case DMV_OP_MIN:
            case DMV_OP_MAX:
            {
                uint16_t d = rd16(insn + OPOFF(ADD, 0));
                int32_t a = read_int(v, d), n = v32(v, insn, 1, OPOFF(ADD, 1)), r;
                switch (insn[0])
                {
                    case DMV_OP_ADD: r = (int32_t)((uint32_t)a + (uint32_t)n); break;
                    case DMV_OP_SUB: r = (int32_t)((uint32_t)a - (uint32_t)n); break;
                    case DMV_OP_MUL: r = (int32_t)((uint32_t)a * (uint32_t)n); break;
                    case DMV_OP_DIV: r = (n == 0 || (n == -1 && a == INT32_MIN)) ? ((n == 0) ? 0 : a) : a / n; break;
                    case DMV_OP_MOD: r = (n == 0 || n == -1) ? 0 : a % n; break;
                    case DMV_OP_MIN: r = (a < n) ? a : n; break;
                    default:         r = (a > n) ? a : n; break;
                }
                view_set_int(v, d, r);
                break;
            }
            case DMV_OP_CLAMP:
            {
                uint16_t d = rd16(insn + OPOFF(CLAMP, 0));
                int32_t a = read_int(v, d), lo = v32(v, insn, 1, OPOFF(CLAMP, 1)), hi = v32(v, insn, 2, OPOFF(CLAMP, 2));
                view_set_int(v, d, (a < lo) ? lo : ((a > hi) ? hi : a));
                break;
            }
            case DMV_OP_TOGGLE:
            {
                uint16_t d = rd16(insn + OPOFF(TOGGLE, 0));
                view_set_int(v, d, (read_int(v, d) == 0) ? 1 : 0);
                break;
            }
            case DMV_OP_FORMAT:
            {
                char text[64];
                format_into(text, sizeof(text), vstr(v, insn, 1, OPOFF(FORMAT, 1)), v32(v, insn, 2, OPOFF(FORMAT, 2)));
                view_set_string(v, rd16(insn + OPOFF(FORMAT, 0)), text);
                break;
            }

            /* ---- Input ---- */
            case DMV_OP_ON:
                if (v->drawing && f->box != ROOT)
                    v->boxes[f->box].handlers[rd16(insn + OPOFF(ON, 0))] = rd16(insn + OPOFF(ON, 1));
                break;

            /* ---- Actions (handlers only) ---- */
            case DMV_OP_REDRAW:
                if (!v->drawing)
                {
                    uint16_t box = rd16(insn + OPOFF(REDRAW, 0));
                    view_mark_dirty(v, (box == DMV_NONE) ? context_box(v) : (int32_t)box);
                }
                break;
            case DMV_OP_EXEC:
                if (!v->drawing)
                    run_command(vstr(v, insn, 0, OPOFF(EXEC, 0)));
                break;
            case DMV_OP_SIGNAL:
                if (!v->drawing)
                    (void)dmhaman_call_handler(vstr(v, insn, 0, OPOFF(SIGNAL, 0)), NULL);
                break;
            case DMV_OP_GOTO:
                if (!v->drawing)
                    set_goto(v, vstr(v, insn, 0, OPOFF(GOTO, 0)));
                break;
            case DMV_OP_SCROLLTO:
                if (!v->drawing)
                    scroll_to(v, rd16(insn + OPOFF(SCROLLTO, 0)), v16(v, insn, 1, OPOFF(SCROLLTO, 1)),
                              v16(v, insn, 2, OPOFF(SCROLLTO, 2)));
                break;
            case DMV_OP_RELOAD:
            case DMV_OP_SETFOCUS:
                break;

            default:
                return;
        }
        pc = next;
    }
    DMOD_LOG_ERROR("libdmview: stopped a view running more than %u instructions\n", (unsigned)MAX_STEPS);
}

void exec_handler(struct libdmview* v, int32_t box, uint16_t label)
{
    v->drawing = false;
    v->handler_box = box;
    v->depth = 0;
    run(v, label * DMV_CODE_WORD, false);
}

/* Draw `area` of the region of box `target` (ROOT: the whole view). */
static void redraw(struct libdmview* v, int32_t target, const rect_t* area)
{
    rect_t screen = { 0, 0, v->surface_w, v->surface_h };
    frame_t* f = &v->frames[0];
    v->depth = 0;

    if (target == ROOT)
    {
        /* Everything is executed again (only `area` is drawn into) */
        for (uint32_t i = 0; i < v->box_count; i++)
            v->boxes[i].flags &= (uint8_t)~(BOXF_VISIBLE | BOXF_DIRTY);
        memset(v->deps, 0, (v->box_count + 1U) * v->dep_words * sizeof(uint32_t));
        f->ox = 0;
        f->oy = 0;
        f->clip = rect_and(screen, area);
        f->view_clip = screen;
        f->box = ROOT;
        run(v, v->entry * DMV_CODE_WORD, false);
        return;
    }

    rbox_t* b = &v->boxes[target];
    for (uint32_t i = 0; i < v->box_count; i++)
    {
        if (v->boxes[i].begin >= b->begin && v->boxes[i].begin <= b->end)
            v->boxes[i].flags &= (uint8_t)~BOXF_VISIBLE;
    }
    f->ox = b->parent_ox;
    f->oy = b->parent_oy;
    f->clip = rect_and(rect_and(b->parent_clip, area), &screen);
    f->view_clip = b->parent_clip;
    f->box = b->parent;
    run(v, b->begin * DMV_CODE_WORD, true);
}

void exec_draw(struct libdmview* v, rect_t* changed)
{
    rect_t screen = { 0, 0, v->surface_w, v->surface_h };
    changed->x0 = changed->y0 = changed->x1 = changed->y1 = 0;
    v->drawing = true;

    if (v->full)
    {
        v->full = false;
        v->any_dirty = false;
        redraw(v, ROOT, &screen);
        *changed = screen;
    }
    else if (v->any_dirty)
    {
        rect_t root_area = { 0, 0, 0, 0 };
        v->any_dirty = false;
        for (uint32_t i = 0; i < v->box_count && !v->full; i++)
        {
            rbox_t* b = &v->boxes[i];
            if ((b->flags & BOXF_DIRTY) == 0)
                continue;
            if ((b->flags & BOXF_VISIBLE) == 0)
            {
                b->flags &= (uint8_t)~BOXF_DIRTY;
                continue;
            }

            /* What lies beneath a box that does not cover itself - or that
             * moves - is its parent's: redraw upwards to an opaque box,
             * clipped to the area that changed */
            int32_t target = (int32_t)i;
            rect_t area = b->bounds;
            if ((b->flags & BOXF_GEOMETRY_VAR) != 0)
            {
                target = b->parent;
                area = (target == ROOT) ? screen : v->boxes[target].bounds;
            }
            while (target != ROOT && (v->boxes[target].flags & DMV_BOX_OPAQUE) == 0)
                target = v->boxes[target].parent;
            area = rect_and(area, &screen);
            if (target == ROOT)
            {
                /* Collected: the whole view is executed once for all of them */
                b->flags &= (uint8_t)~BOXF_DIRTY;
                rect_add(&root_area, &area);
                continue;
            }
            redraw(v, target, &area);
            rect_add(changed, &area);
        }
        if (v->full)
        {
            v->full = false;
            redraw(v, ROOT, &screen);
            *changed = screen;
        }
        else if (!rect_empty(&root_area))
        {
            redraw(v, ROOT, &root_area);
            rect_add(changed, &root_area);
        }
    }
    v->drawing = false;
}
