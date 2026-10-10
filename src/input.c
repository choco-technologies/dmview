#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Rendering, input events and time.
 *
 * Input is runtime level 1 of docs/assembly.md: one contact (the first one
 * of the device's state). The box it touches first - the topmost visible box
 * under it with a pointer handler, or the nearest parent with one - captures
 * it until it is lifted.
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
           b->handlers[DMV_EVENT_CLICK] != DMV_NONE;
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

dmod_libdmview_api_declaration(1.0, int, _input, ( libdmview_t view, const dmdrvi_input_state_t* state, uint32_t now_ms ))
{
    if (!is_view(view) || state == NULL)
        return -EINVAL;
    struct libdmview* v = view;
    set_now(v, now_ms);

    bool down = state->contact_count > 0;
    int32_t x = down ? state->contacts[0].x : v->last_x;
    int32_t y = down ? state->contacts[0].y : v->last_y;
    v->ev[DMV_VAR_EV_CONTACT - DMV_VAR_EV_CONTACT] = down ? state->contacts[0].id : 0;

    if (down && !v->down)
    {
        v->down = true;
        v->long_fired = false;
        v->press_ms = now_ms;
        v->last_x = x;
        v->last_y = y;
        v->captured = hit_test(v, x, y);
        pressed_changed(v, v->captured);
        fire(v, v->captured, DMV_EVENT_PRESS, x, y, 0, 0);
    }
    else if (down && (x != v->last_x || y != v->last_y))
    {
        int32_t dx = x - v->last_x, dy = y - v->last_y;
        v->last_x = x;
        v->last_y = y;
        fire(v, v->captured, DMV_EVENT_DRAG, x, y, dx, dy);
    }
    else if (!down && v->down)
    {
        int32_t box = v->captured;
        v->down = false;
        v->captured = ROOT;
        pressed_changed(v, box);
        if (box != ROOT)
        {
            /* The box still receives its contact's RELEASE and CLICK */
            int32_t held = v->captured;
            v->captured = box;
            fire(v, box, DMV_EVENT_RELEASE, x, y, 0, 0);
            if (contains(&v->boxes[box].clip, x, y))
                fire(v, box, DMV_EVENT_CLICK, x, y, 0, 0);
            v->captured = held;
        }
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
    return 0;
}

dmod_libdmview_api_declaration(1.0, uint32_t, _update, ( libdmview_t view, uint32_t now_ms ))
{
    if (!is_view(view))
        return LIBDMVIEW_NO_DEADLINE;
    struct libdmview* v = view;
    set_now(v, now_ms);
    uint32_t t = now_ms - v->start_ms, next = LIBDMVIEW_NO_DEADLINE;

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

    if (v->down && v->captured != ROOT && !v->long_fired)
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
    return next;
}
