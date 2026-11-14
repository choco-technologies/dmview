#include "private.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/*
 * Composing the rents of a display (see libdmview.h, "Screen rent"): the service shows the view of the claim on top as before,
 * and a libdmview_rents_t holds the views of the applications that took its rectangles. It draws them over the owner's view -
 * each into the part of the same framebuffer that is its rectangle, as a surface of its own size, so it sees `$view_w`/`$view_h`
 * of its part and nothing else - and gives them the touches and keys that are theirs.
 */

#define TARGET_OWNER    1u
#define TARGET_ENTRY    2u

typedef struct
{
    uint32_t            id;             /* The rent */
    uint32_t            claim;          /* The application's claim, 0: nobody */
    char                path[LIBDMVIEW_PATH_MAX];
    libdmview_t         view;           /* NULL: nobody, or not loadable (not tried again until the claim or the view changes) */
    libdmview_rect_t    rect;
    bool                visible;
    bool                focus;
    uint16_t            escape;
    bool                touching;       /* Got contacts last time: it is told when they are gone */
} entry_t;

#define LIVENESS_MS     500u        /* How often the processes of the claims are looked at (see _rents_sync) */

struct libdmview_rents
{
    libdmview_display_t display;
    uint32_t            generation;     /* Of the rents of the display, as last taken in */
    uint32_t            display_generation;
    bool                loaded;         /* Taken in at least once */
    uint32_t            checked_ms;     /* When the processes of the claims were last looked at */
    entry_t             entries[LIBDMVIEW_RENTS_MAX];
    uint32_t            count;
    uint8_t             target[256];    /* Per contact id: 0 - not down, TARGET_OWNER, or TARGET_ENTRY + the entry that has it */

    /* What the functions below need while they work: here, not on the stack of the service - they are called from its loop, and a
     * stack of a few kB is what it has (they are one thread's, the service's) */
    libdmview_rent_info_t   infos[LIBDMVIEW_RENTS_MAX];
    entry_t                 next[LIBDMVIEW_RENTS_MAX];
    dmdrvi_input_state_t    parts[LIBDMVIEW_RENTS_MAX];
    bool                    has[LIBDMVIEW_RENTS_MAX];
    bool                    seen[256];
};

dmod_libdmview_api_declaration(1.0, libdmview_rents_t, _rents_create, ( libdmview_display_t display ))
{
    if (display == NULL)
        return NULL;
    struct libdmview_rents* r = Dmod_Malloc(sizeof(*r));
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof(*r));
    r->display = display;
    return r;
}

static void entry_close(entry_t* e)
{
    libdmview_close(e->view);
    memset(e, 0, sizeof(*e));
}

dmod_libdmview_api_declaration(1.0, void, _rents_destroy, ( libdmview_rents_t rents ))
{
    if (rents == NULL)
        return;
    for (uint32_t i = 0; i < rents->count; i++)
        entry_close(&rents->entries[i]);
    Dmod_Free(rents);
}

static void rect_union_add(libdmview_rect_t* to, const libdmview_rect_t* r)
{
    if (r->w == 0 || r->h == 0)
        return;
    if (to->w == 0 || to->h == 0)
    {
        *to = *r;
        return;
    }
    int32_t x0 = (to->x < r->x) ? to->x : r->x, y0 = (to->y < r->y) ? to->y : r->y;
    int32_t x1 = (to->x + to->w > r->x + r->w) ? to->x + to->w : r->x + r->w;
    int32_t y1 = (to->y + to->h > r->y + r->h) ? to->y + to->h : r->y + r->h;
    to->x = (int16_t)x0;
    to->y = (int16_t)y0;
    to->w = (uint16_t)(x1 - x0);
    to->h = (uint16_t)(y1 - y0);
}

/* A rent shows its application: it has a view, and is shown */
static bool shown(const entry_t* e)
{
    return e->view != NULL && e->visible && e->rect.w != 0 && e->rect.h != 0;
}

dmod_libdmview_api_declaration(1.0, bool, _rents_sync, ( libdmview_rents_t rents, libdmview_rect_t* vacated ))
{
    if (vacated != NULL)
        memset(vacated, 0, sizeof(*vacated));
    if (rents == NULL)
        return false;

    /* The rents of the claim on top move with the claims too (another claim on top: other rents) */
    uint32_t generation = 0;
    uint32_t display_generation = libdmview_display_generation(rents->display);
    /* Looking at the processes of the claims (is the application still there?) is not cheap - on the ESP32-S3 the scan of the tasks takes tens of
     * milliseconds for every claim - and this is called by every turn of the loop of the service: between the looks only a change of the claims or
     * of the rents is taken in, which is what this is for. An application that ends is noticed within LIBDMVIEW_LIVENESS_MS. */
    const uint32_t now = dmosi_get_tick_count();
    if (rents->loaded && (uint32_t)(now - rents->checked_ms) < LIVENESS_MS &&
        libdmview_display_rent_generation(rents->display) == rents->generation && display_generation == rents->display_generation)
        return false;
    rents->checked_ms = now;
    libdmview_rent_info_t* infos = rents->infos;
    uint32_t count = 0;
    if (libdmview_display_rents(rents->display, &generation, infos, LIBDMVIEW_RENTS_MAX, &count) != 0)
        return false;
    if (rents->loaded && generation == rents->generation && display_generation == rents->display_generation)
        return false;
    rents->loaded = true;
    rents->generation = generation;
    rents->display_generation = display_generation;

    entry_t* next = rents->next;
    memset(next, 0, sizeof(rents->next));
    bool changed = false;
    libdmview_rect_t gone = { 0, 0, 0, 0 };

    for (uint32_t i = 0; i < count; i++)
    {
        const libdmview_rent_info_t* info = &infos[i];
        entry_t* n = &next[i];
        n->id = info->id;
        n->claim = info->claim;
        n->rect = info->rect;
        n->visible = info->visible;
        n->focus = info->focus;
        n->escape = info->escape;
        strcpy(n->path, info->path);

        /* The same rent, the same application, the same view: its view is kept */
        entry_t* old = NULL;
        for (uint32_t k = 0; k < rents->count; k++)
        {
            if (rents->entries[k].id == info->id && rents->entries[k].claim == info->claim &&
                strcmp(rents->entries[k].path, info->path) == 0)
            {
                old = &rents->entries[k];
                break;
            }
        }
        if (old != NULL)
        {
            n->view = old->view;
            n->touching = old->touching;
            old->view = NULL;
            /* Where it was and is not: the owner's again (a resize that grows draws the new part itself) */
            bool was = old->visible && old->rect.w != 0 && n->view != NULL;
            bool moved = old->rect.x != n->rect.x || old->rect.y != n->rect.y;
            bool smaller = old->rect.w > n->rect.w || old->rect.h > n->rect.h;
            if (was && (!n->visible || moved || smaller))
                rect_union_add(&gone, &old->rect);
            if (old->visible != n->visible || moved || old->rect.w != n->rect.w || old->rect.h != n->rect.h ||
                old->focus != n->focus || old->escape != n->escape)
                changed = true;
        }
        else
        {
            changed = true;
            if (n->claim != 0 && n->path[0] != '\0')
            {
                int status = 0;
                n->view = libdmview_open(n->path, &status);
                if (n->view != NULL)
                {
                    (void)libdmview_rent_attach(n->view, n->claim);
                    DMOD_LOG_INFO("libdmview: %s is shown in the rent %u (%ux%u at %d,%d)\n", n->path, (unsigned)n->id,
                                  (unsigned)n->rect.w, (unsigned)n->rect.h, (int)n->rect.x, (int)n->rect.y);
                }
                else
                    DMOD_LOG_ERROR("dmview: cannot show %s in a rent (%d)\n", n->path, status);
            }
        }
    }

    /* The contacts that began in a rent follow it to its new place in the list (or are the owner's when it is gone) */
    uint8_t remap[LIBDMVIEW_RENTS_MAX];
    memset(remap, TARGET_OWNER, sizeof(remap));
    for (uint32_t k = 0; k < rents->count; k++)
    {
        for (uint32_t j = 0; j < count; j++)
        {
            if (next[j].id == rents->entries[k].id)
                remap[k] = (uint8_t)(TARGET_ENTRY + j);
        }
    }
    for (uint32_t t = 0; t < sizeof(rents->target); t++)
    {
        uint8_t v = rents->target[t];
        if (v >= TARGET_ENTRY)
            rents->target[t] = (v - TARGET_ENTRY < LIBDMVIEW_RENTS_MAX) ? remap[v - TARGET_ENTRY] : (uint8_t)TARGET_OWNER;
    }

    /* What went: the views of rents that are no more, or whose application or view is another */
    for (uint32_t k = 0; k < rents->count; k++)
    {
        entry_t* old = &rents->entries[k];
        if (old->view != NULL)
        {
            if (old->visible && old->rect.w != 0)
                rect_union_add(&gone, &old->rect);
            changed = true;
        }
        entry_close(old);
    }
    memcpy(rents->entries, next, sizeof(rents->entries));
    rents->count = count;

    /* ... and one that began in a rent that is not shown any more is the owner's */
    for (uint32_t t = 0; t < sizeof(rents->target); t++)
    {
        if (rents->target[t] >= TARGET_ENTRY &&
            (rents->target[t] - TARGET_ENTRY >= count || !shown(&rents->entries[rents->target[t] - TARGET_ENTRY])))
            rents->target[t] = TARGET_OWNER;
    }
    if (vacated != NULL)
        *vacated = gone;
    return changed;
}

dmod_libdmview_api_declaration(1.0, uint32_t, _rents_update, ( libdmview_rents_t rents, uint32_t now_ms ))
{
    uint32_t deadline = LIBDMVIEW_NO_DEADLINE;
    for (uint32_t i = 0; rents != NULL && i < rents->count; i++)
    {
        if (!shown(&rents->entries[i]))
            continue;
        uint32_t d = libdmview_update(rents->entries[i].view, now_ms);
        if (d < deadline)
            deadline = d;
    }
    return deadline;
}

/* The part of `surface` that is `rect` (clamped to it), as a surface; false when none of it is on the surface */
static bool sub_surface(const libdmview_surface_t* surface, const libdmview_rect_t* rect, libdmview_surface_t* out, int32_t* x0, int32_t* y0)
{
    int32_t x = rect->x, y = rect->y;
    int32_t x1 = x + rect->w, y1 = y + rect->h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > surface->width) x1 = surface->width;
    if (y1 > surface->height) y1 = surface->height;
    if (x1 <= x || y1 <= y)
        return false;
    const uint32_t bpp = (surface->format == DMDRVI_GFX_PIXEL_FORMAT_ARGB8888) ? 4U : 2U;
    out->pixels = (uint8_t*)surface->pixels + (size_t)y * surface->stride + (size_t)x * bpp;
    out->width = (uint16_t)(x1 - x);
    out->height = (uint16_t)(y1 - y);
    out->stride = surface->stride;
    out->format = surface->format;
    *x0 = x;
    *y0 = y;
    return true;
}

dmod_libdmview_api_declaration(1.0, void, _rents_damage, ( libdmview_rents_t rents, const libdmview_rect_t* area ))
{
    if (rents == NULL || area == NULL || area->w == 0 || area->h == 0)
        return;
    for (uint32_t i = 0; i < rents->count; i++)
    {
        entry_t* e = &rents->entries[i];
        if (!shown(e))
            continue;
        int32_t x0 = (area->x > e->rect.x) ? area->x : e->rect.x;
        int32_t y0 = (area->y > e->rect.y) ? area->y : e->rect.y;
        int32_t x1 = (area->x + area->w < e->rect.x + e->rect.w) ? area->x + area->w : e->rect.x + e->rect.w;
        int32_t y1 = (area->y + area->h < e->rect.y + e->rect.h) ? area->y + area->h : e->rect.y + e->rect.h;
        if (x1 <= x0 || y1 <= y0)
            continue;
        libdmview_rect_t local = { (int16_t)(x0 - e->rect.x), (int16_t)(y0 - e->rect.y), (uint16_t)(x1 - x0), (uint16_t)(y1 - y0) };
        libdmview_invalidate_rect(e->view, &local);
    }
}

dmod_libdmview_api_declaration(1.0, int, _rents_render, ( libdmview_rents_t rents, const libdmview_surface_t* surface, libdmview_rect_t* changed ))
{
    if (changed != NULL)
        memset(changed, 0, sizeof(*changed));
    if (rents == NULL || surface == NULL)
        return -EINVAL;
    libdmview_rect_t all = { 0, 0, 0, 0 };
    int result = 0;
    for (uint32_t i = 0; i < rents->count; i++)
    {
        entry_t* e = &rents->entries[i];
        libdmview_surface_t part;
        int32_t x0, y0;
        if (!shown(e) || !sub_surface(surface, &e->rect, &part, &x0, &y0))
            continue;
        libdmview_rect_t drawn;
        int ret = libdmview_render(e->view, &part, &drawn);
        if (ret > 0)
        {
            drawn.x = (int16_t)(drawn.x + x0);
            drawn.y = (int16_t)(drawn.y + y0);
            rect_union_add(&all, &drawn);
            if (result == 0)
                result = 1;
        }
        else if (ret < 0)
            result = ret;
    }
    if (changed != NULL)
        *changed = all;
    return result;
}

/* The shown rent a point is in, or -1 */
static int32_t hit(const libdmview_rents_t rents, int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < rents->count; i++)
    {
        const entry_t* e = &rents->entries[i];
        if (shown(e) && x >= e->rect.x && x < e->rect.x + e->rect.w && y >= e->rect.y && y < e->rect.y + e->rect.h)
            return (int32_t)i;
    }
    return -1;
}

dmod_libdmview_api_declaration(1.0, bool, _rents_input, ( libdmview_rents_t rents, dmdrvi_input_state_t* state, uint32_t now_ms ))
{
    if (rents == NULL || state == NULL || rents->count == 0)
        return false;

    dmdrvi_input_state_t* parts = rents->parts;
    bool* has = rents->has;
    memset(rents->parts, 0, sizeof(rents->parts));
    memset(rents->has, 0, sizeof(rents->has));
    dmdrvi_input_state_t owner = *state;
    owner.contact_count = 0;
    memset(owner.contacts, 0, sizeof(owner.contacts));

    /* A contact belongs to what it began in until it is lifted - by its UP, or by being missing from the state (a driver that
     * reports no UP, or none that is not a DOWN, is served too) */
    bool* seen = rents->seen;
    memset(rents->seen, 0, sizeof(rents->seen));
    uint32_t n = (state->contact_count < DMDRVI_INPUT_MAX_CONTACTS) ? state->contact_count : DMDRVI_INPUT_MAX_CONTACTS;
    for (uint32_t i = 0; i < n; i++)
    {
        dmdrvi_input_contact_t c = state->contacts[i];
        seen[c.id] = true;
        if (rents->target[c.id] == 0)
        {
            int32_t h = hit(rents, c.x, c.y);
            rents->target[c.id] = (h < 0) ? (uint8_t)TARGET_OWNER : (uint8_t)(TARGET_ENTRY + (uint32_t)h);
        }
        uint8_t t = rents->target[c.id];
        if (c.event == DMDRVI_INPUT_CONTACT_UP)
            rents->target[c.id] = 0;
        if (t < TARGET_ENTRY || t - TARGET_ENTRY >= rents->count)
        {
            owner.contacts[owner.contact_count++] = c;
            continue;
        }
        const uint32_t k = t - TARGET_ENTRY;
        const entry_t* e = &rents->entries[k];
        dmdrvi_input_state_t* part = &parts[k];
        c.x = (uint16_t)(c.x - e->rect.x);                  /* relative to the rent */
        c.y = (uint16_t)(c.y - e->rect.y);
        part->contacts[part->contact_count++] = c;
        has[k] = true;
    }
    for (uint32_t id = 0; id < sizeof(rents->seen); id++)
    {
        if (!seen[id])
            rents->target[id] = 0;                          /* (lifted) */
    }

    bool routed = false;
    for (uint32_t i = 0; i < rents->count; i++)
    {
        entry_t* e = &rents->entries[i];
        if (!shown(e))
            continue;
        if (has[i] || e->touching)
        {
            (void)libdmview_input(e->view, &parts[i], now_ms);      /* (an empty state ends what was begun) */
            e->touching = has[i];
            routed = routed || has[i];
        }
    }
    *state = owner;
    return routed;
}

dmod_libdmview_api_declaration(1.0, bool, _rents_key, ( libdmview_rents_t rents, uint16_t code, uint32_t codepoint, bool down, uint8_t modifiers ))
{
    for (uint32_t i = 0; rents != NULL && i < rents->count; i++)
    {
        const entry_t* e = &rents->entries[i];
        if (!shown(e) || !e->focus)
            continue;
        if (e->escape != 0 && e->escape == code)
            return false;                                       /* It is the owner's */
        (void)libdmview_key(e->view, code, codepoint, down, modifiers);
        return true;
    }
    return false;
}
