#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Rendering, input events and time.
 *
 * Input captures a primary contact, with a second contact for PINCH on
 * MULTI boxes. Independent simultaneous controls and ROTATE are not implemented.
 * The topmost visible box under the primary contact, or its nearest parent
 * with a pointer handler, captures it until it is lifted.
 */

static bool is_view(libdmview_t v)
{
    return v != NULL && v->magic == VIEW_MAGIC;
}

dmod_libdmview_api_declaration(1.0, int, _render, ( libdmview_t view, const libdmview_surface_t* surface, libdmview_rect_t* changed ))
{
    if (!is_view(view) || surface == NULL || surface->pixels == NULL)
        return -EINVAL;
    if (!draw_supported(surface->format))
        return -ENOTSUP;
    if (surface->width != view->surface_w || surface->height != view->surface_h)
    {
        view->surface_w = surface->width;
        view->surface_h = surface->height;
        view->full = true;
    }

    rect_t c;
    view->surface = surface;
    exec_draw(view, &c);
    view->surface = NULL;

    if (changed != NULL)
    {
        changed->x = (int16_t)c.x0;
        changed->y = (int16_t)c.y0;
        changed->w = (uint16_t)(rect_empty(&c) ? 0 : c.x1 - c.x0);
        changed->h = (uint16_t)(rect_empty(&c) ? 0 : c.y1 - c.y0);
    }
    return rect_empty(&c) ? 0 : 1;
}

/* ---- Time ---- */

static void set_now(struct libdmview* v, uint32_t now_ms)
{
    if (!v->have_now)
    {
        v->have_now = true;
        v->start_ms = now_ms;
    }
    v->now_ms = now_ms;
}

/* ---- Events ---- */

static bool has_pointer_handler(const rbox_t* b)
{
    return b->handlers[DMV_EVENT_PRESS] != DMV_NONE || b->handlers[DMV_EVENT_DRAG] != DMV_NONE ||
           b->handlers[DMV_EVENT_LONG] != DMV_NONE || b->handlers[DMV_EVENT_RELEASE] != DMV_NONE ||
           b->handlers[DMV_EVENT_CLICK] != DMV_NONE || b->handlers[DMV_EVENT_PINCH] != DMV_NONE;
}

static bool contains(const rect_t* r, int32_t x, int32_t y)
{
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

/* The topmost visible box under (x, y) - boxes are drawn in code order, so
 * the one starting last is on top - or the nearest parent of it that takes
 * the contact. A box not seen (its opacity, or one around it, 0: a screen
 * hidden behind the shown one) takes none - what is seen beneath does. */
static int32_t hit_test(const struct libdmview* v, int32_t x, int32_t y)
{
    int32_t hit = ROOT;
    for (uint32_t i = 0; i < v->box_count; i++)
    {
        const rbox_t* b = &v->boxes[i];
        if ((b->flags & BOXF_VISIBLE) != 0 && b->seen != 0 && contains(&b->clip, x, y) &&
            (hit == ROOT || b->begin > v->boxes[hit].begin))
            hit = (int32_t)i;
    }
    while (hit != ROOT && !has_pointer_handler(&v->boxes[hit]))
        hit = v->boxes[hit].parent;
    return hit;
}

/* Whether a box scrolls along an axis: SCROLL, its content larger than it, the axis allowed */
static bool scrolls(const rbox_t* b, bool vertical)
{
    if ((b->flags & BOXF_VISIBLE) == 0 || b->seen == 0)
        return false;
    uint8_t axis = vertical ? DMV_SCROLL_VERTICAL : DMV_SCROLL_HORIZONTAL;
    bool allowed = (b->scroll_flags & (DMV_SCROLL_VERTICAL | DMV_SCROLL_HORIZONTAL)) == 0 || (b->scroll_flags & axis) != 0;
    return allowed && (vertical ? b->ch > b->bounds.y1 - b->bounds.y0 : b->cw > b->bounds.x1 - b->bounds.x0);
}

/* The scroll box a contact at (x, y) moving along an axis drags: the topmost seen box there, or the nearest box
 * around it that scrolls that way */
static int32_t scroll_box_at(const struct libdmview* v, int32_t x, int32_t y, bool vertical)
{
    int32_t top = ROOT;
    for (uint32_t i = 0; i < v->box_count; i++)
    {
        const rbox_t* b = &v->boxes[i];
        if ((b->flags & BOXF_VISIBLE) != 0 && b->seen != 0 && contains(&b->clip, x, y) &&
            (top == ROOT || b->begin > v->boxes[top].begin))
            top = (int32_t)i;
    }
    while (top != ROOT && !scrolls(&v->boxes[top], vertical))
        top = v->boxes[top].parent;
    return top;
}

static void fire(struct libdmview* v, int32_t box, uint8_t event, int32_t x, int32_t y, int32_t dx, int32_t dy)
{
    uint16_t label = (box != ROOT) ? v->boxes[box].handlers[event] : DMV_NONE;
    if (label == DMV_NONE)
        return;
    const rect_t* b = &v->boxes[box].bounds;
    v->ev[DMV_VAR_EV_X - DMV_VAR_EV_CONTACT] = x - b->x0;
    v->ev[DMV_VAR_EV_Y - DMV_VAR_EV_CONTACT] = y - b->y0;
    v->ev[DMV_VAR_EV_DX - DMV_VAR_EV_CONTACT] = dx;
    v->ev[DMV_VAR_EV_DY - DMV_VAR_EV_CONTACT] = dy;
    exec_handler(v, box, label);
    memset(v->ev, 0, sizeof(v->ev));            /* Event variables are 0 outside handlers */
}

/* $box.pressed / $box.contacts of `box` changed: redraw it if it read them */
static void pressed_changed(struct libdmview* v, int32_t box)
{
    if (box != ROOT && (v->deps[(uint32_t)box * v->dep_words + v->dep_words - 1U] & (DEP_PRESSED | DEP_CONTACTS)) != 0)
        view_mark_dirty(v, box);
}

/* Integer square root: no floating point or libm dependency on targets. */
static uint32_t contact_span(int32_t dx, int32_t dy)
{
    uint64_t n = (uint64_t)((int64_t)dx * dx) + (uint64_t)((int64_t)dy * dy);
    uint64_t result = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0)
    {
        if (n >= result + bit) { n -= result + bit; result = (result >> 1) + bit; }
        else result >>= 1;
        bit >>= 2;
    }
    return (uint32_t)result;
}

static const dmdrvi_input_contact_t* contact_by_id(const dmdrvi_input_state_t* state, uint8_t id)
{
    uint32_t count = state->contact_count;
    if (count > DMDRVI_INPUT_MAX_CONTACTS) count = DMDRVI_INPUT_MAX_CONTACTS;
    for (uint32_t i = 0; i < count; i++)
        if (state->contacts[i].id == id) return &state->contacts[i];
    return NULL;
}

/* A second finger can join the primary capture only on a MULTI box. Once
 * captured it remains there outside its bounds too. Single-contact input
 * and scrolling keep their existing path; no independent second click is
 * invented, and a pinch never turns into a click when the fingers lift. */
static bool pinch_input(struct libdmview* v, const dmdrvi_input_state_t* state, int32_t x, int32_t y)
{
    int32_t box = v->captured;
    if (box == ROOT || (v->boxes[box].flags & DMV_BOX_MULTI) == 0) return false;
    const dmdrvi_input_contact_t* second = v->pinch_active ? contact_by_id(state, v->pinch_id) : NULL;
    if (second == NULL)
    {
        if (v->pinch_active) { v->pinch_active = false; pressed_changed(v, box); }
        uint32_t count = state->contact_count;
        if (count > DMDRVI_INPUT_MAX_CONTACTS) count = DMDRVI_INPUT_MAX_CONTACTS;
        for (uint32_t i = 0; i < count; i++)
        {
            const dmdrvi_input_contact_t* c = &state->contacts[i];
            if (c->id != v->primary_id && hit_test(v, c->x, c->y) == box) { second = c; break; }
        }
        if (second == NULL) return false;
    }
    int32_t dx = (int32_t)second->x - x, dy = (int32_t)second->y - y;
    int32_t cx = (x + second->x) / 2, cy = (y + second->y) / 2;
    uint32_t span = contact_span(dx, dy);
    if (!v->pinch_active)
    {
        v->pinch_active = true;
        v->pinch_used = true;
        v->pinch_id = second->id;
        v->pinch_span = span;
        v->pinch_dx = dx; v->pinch_dy = dy; v->pinch_cx = cx; v->pinch_cy = cy;
        pressed_changed(v, box);
        return true;
    }
    if (v->pinch_span == 0) v->pinch_span = span; /* Coincident fingers: establish a nonzero baseline first. */
    if (v->pinch_span != 0 && (dx != v->pinch_dx || dy != v->pinch_dy || cx != v->pinch_cx || cy != v->pinch_cy))
    {
        v->ev[DMV_VAR_EV_CONTACT - DMV_VAR_EV_CONTACT] = v->primary_id;
        v->ev[DMV_VAR_EV_SCALE - DMV_VAR_EV_CONTACT] = (int32_t)((uint64_t)span * 1000u / v->pinch_span);
        v->ev[DMV_VAR_EV_CX - DMV_VAR_EV_CONTACT] = cx - v->boxes[box].bounds.x0;
        v->ev[DMV_VAR_EV_CY - DMV_VAR_EV_CONTACT] = cy - v->boxes[box].bounds.y0;
        fire(v, box, DMV_EVENT_PINCH, x, y, 0, 0);
    }
    v->pinch_dx = dx; v->pinch_dy = dy; v->pinch_cx = cx; v->pinch_cy = cy;
    return true;
}

/* A key of a keyboard goes to the application of the claim (a view has no text fields): see libdmview.h */
dmod_libdmview_api_declaration(1.0, int, _key, ( libdmview_t view, uint16_t code, uint32_t codepoint, bool down, uint8_t modifiers ))
{
    if (!is_view(view))
        return -EINVAL;
    bool changed = modifiers != view->key_mods;
    if (changed)
    {
        view->key_mods = modifiers;
        claims_emit(view, "mods", (int32_t)modifiers);
    }
    if (!down)
        return 0;
    bool modifier_key = (code >= 0xE0u && code <= 0xE7u) || (codepoint == 0 && changed);   /* (it gave "mods") */
    if (codepoint != 0)
        claims_emit(view, "char", (int32_t)codepoint);
    else if (!modifier_key)
        claims_emit(view, "key", (int32_t)code);
    return 0;
}

dmod_libdmview_api_declaration(1.0, int, _input, ( libdmview_t view, const dmdrvi_input_state_t* state, uint32_t now_ms ))
{
    if (!is_view(view) || state == NULL)
        return -EINVAL;
    struct libdmview* v = view;
    set_now(v, now_ms);

    const dmdrvi_input_contact_t* primary = v->down ? contact_by_id(state, v->primary_id) : NULL;
    if (primary == NULL && state->contact_count > 0)
    {
        primary = &state->contacts[0];
        if (v->down && v->pinch_used)
        {
            v->last_x = v->start_x = primary->x;
            v->last_y = v->start_y = primary->y;
            v->pinch_active = false;
            pressed_changed(v, v->captured);
        }
        v->primary_id = primary->id;
    }
    bool down = primary != NULL;
    int32_t x = down ? primary->x : v->last_x;
    int32_t y = down ? primary->y : v->last_y;
    v->ev[DMV_VAR_EV_CONTACT - DMV_VAR_EV_CONTACT] = down ? primary->id : v->primary_id;

    if (down && !v->down)
    {
        v->down = true;
        v->long_fired = false;
        v->press_ms = now_ms;
        v->last_x = x;
        v->last_y = y;
        v->start_x = x;
        v->start_y = y;
        v->scrolling = ROOT;
        v->captured = hit_test(v, x, y);
        pressed_changed(v, v->captured);
        fire(v, v->captured, DMV_EVENT_PRESS, x, y, 0, 0);
        (void)pinch_input(v, state, x, y);
    }
    else if (down && pinch_input(v, state, x, y))
    {
        /* Rebase the single finger path throughout the gesture. */
        v->last_x = v->start_x = x;
        v->last_y = v->start_y = y;
    }
    else if (down && (x != v->last_x || y != v->last_y))
    {
        int32_t dx = x - v->last_x, dy = y - v->last_y;
        v->last_x = x;
        v->last_y = y;
        int32_t mx = x - v->start_x, my = y - v->start_y;
        int32_t ax = (mx < 0) ? -mx : mx, ay = (my < 0) ? -my : my;
        if (v->scrolling == ROOT && (ax > (int32_t)v->scrollslop || ay > (int32_t)v->scrollslop))
        {
            /* Past the threshold: a scroll box around the contact takes it over - the box pressed is released, not clicked */
            int32_t box = scroll_box_at(v, v->start_x, v->start_y, ay >= ax);
            if (box == ROOT)
                box = scroll_box_at(v, v->start_x, v->start_y, ay < ax);
            if (box != ROOT)
            {
                int32_t held = v->captured;
                v->scrolling = box;
                v->scroll_sx = v->boxes[box].sx;
                v->scroll_sy = v->boxes[box].sy;
                v->captured = ROOT;
                pressed_changed(v, held);
                fire(v, held, DMV_EVENT_RELEASE, x, y, 0, 0);
            }
        }
        if (v->scrolling != ROOT)
            view_scroll_to(v, (uint16_t)v->scrolling, v->scroll_sx - mx, v->scroll_sy - my);
        else
            fire(v, v->captured, DMV_EVENT_DRAG, x, y, dx, dy);
    }
    else if (!down && v->down)
    {
        int32_t box = v->captured;
        v->down = false;
        v->scrolling = ROOT;
        v->captured = ROOT;
        pressed_changed(v, box);
        if (box != ROOT)
        {
            /* The box still receives its contact's RELEASE and CLICK */
            int32_t held = v->captured;
            v->captured = box;
            fire(v, box, DMV_EVENT_RELEASE, x, y, 0, 0);
            if (!v->pinch_used && contains(&v->boxes[box].clip, x, y))
                fire(v, box, DMV_EVENT_CLICK, x, y, 0, 0);
            v->captured = held;
        }
    }

    if (!down)
    {
        v->pinch_active = false;
        v->pinch_used = false;
    }
    /* .key: buttons that went down */
    uint32_t pressed = state->buttons & ~v->buttons;
    v->buttons = state->buttons;
    for (uint32_t i = 0; pressed != 0 && i < v->item_count; i++)
    {
        if (v->items[i].kind == DMV_ITEM_KEY && (pressed & (1u << v->items[i].value)) != 0)
        {
            v->ev[DMV_VAR_EV_KEY - DMV_VAR_EV_CONTACT] = (int32_t)v->items[i].value;
            exec_handler(v, ROOT, v->items[i].label);
            memset(v->ev, 0, sizeof(v->ev));
        }
    }
    view_fire_scrolled(v);
    return 0;
}

/*
 * SCROLLED of the boxes whose offset changed - by a drag, SCROLLTO, ... -,
 * after what changed it. A handler that scrolls a box again (a list that
 * loads more where it ends, moving what is shown back) gets it once more,
 * a few rounds at most.
 */
void view_fire_scrolled(struct libdmview* v)
{
    for (uint32_t round = 0; v->any_scrolled && round < 4u; round++)
    {
        v->any_scrolled = false;
        for (uint32_t i = 0; i < v->box_count; i++)
        {
            if (!v->boxes[i].scrolled)
                continue;
            v->boxes[i].scrolled = false;
            fire(v, (int32_t)i, DMV_EVENT_SCROLLED, v->last_x, v->last_y, 0, 0);
        }
    }
    v->any_scrolled = false;
    for (uint32_t i = 0; i < v->box_count; i++)
        v->boxes[i].scrolled = false;
}

dmod_libdmview_api_declaration(1.0, uint32_t, _update, ( libdmview_t view, uint32_t now_ms ))
{
    if (!is_view(view))
        return LIBDMVIEW_NO_DEADLINE;
    struct libdmview* v = view;
    set_now(v, now_ms);
    uint32_t t = now_ms - v->start_ms, next = LIBDMVIEW_NO_DEADLINE;

    /* What the application set through the claim, and the .change handlers */
    claims_apply(v);
    /* What waited for an image the loader thread read */
    images_poll(v);

    for (uint32_t i = 0; i < v->item_count; i++)
    {
        item_t* item = &v->items[i];
        if (item->kind != DMV_ITEM_TIMER)
            continue;
        if ((int32_t)(t - item->due) >= 0)
        {
            exec_handler(v, ROOT, item->label);
            item->due += item->value;
            if ((int32_t)(t - item->due) >= 0)      /* Fell behind: no burst of runs */
                item->due = t + item->value;
        }
        uint32_t wait = item->due - t;
        if (wait < next)
            next = wait;
    }

    if (v->down && v->captured != ROOT && !v->long_fired && !v->pinch_used)
    {
        uint32_t held = now_ms - v->press_ms;
        if (held >= v->longpress_ms)
        {
            v->long_fired = true;
            fire(v, v->captured, DMV_EVENT_LONG, v->last_x, v->last_y, 0, 0);
        }
        else if (v->longpress_ms - held < next)
            next = v->longpress_ms - held;
    }
    view_fire_scrolled(v);
    return next;
}
