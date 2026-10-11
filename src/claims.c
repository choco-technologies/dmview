#include "private.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/*
 * Display registry - the one state libdmview shares: services register
 * their displays, applications claim them for a view while they run.
 *
 * A claim remembers the pid of the process that made it, never a callback
 * or a pointer into it: when the process is gone, its claim is dropped the
 * next time the display is asked (libdmview_display_view()), and the display
 * goes back to the previous claim or the service's own view.
 *
 * A claim is also the channel between the application and its view: the
 * view's properties - read from the view file when it is claimed - and the
 * events it emits. The application writes properties into the claim; the
 * service's thread takes them into the view attached to the claim
 * (claims_apply(), from libdmview_update()), which is the only thread that
 * touches the view. Everything is found by the claim's id under g_lock -
 * nobody keeps a pointer to a claim but a thread waiting for its events,
 * which holds it (`waiters`) until it is done.
 */

#define DISPLAY_MAGIC   0x44535059u     /* 'DSPY' */

typedef struct
{
    char*       name;
    uint8_t     type;           /* dmv_var_type_t */
    uint16_t    capacity;       /* Strings */
    int32_t     ival;
    char*       sval;           /* capacity + 1 bytes */
    bool        pending;        /* Set by the application, not in the view yet */
} prop_t;

typedef struct claim
{
    uint32_t                    id;
    dmosi_process_id_t          pid;
    char*                       path;
    prop_t*                     props;
    uint32_t                    prop_count;
    bool                        pending;        /* A property is */

    libdmview_event_t           events[LIBDMVIEW_EVENT_QUEUE];
    uint32_t                    first, count;   /* Queue */
    uint32_t                    dropped;        /* Since the last event taken */
    dmosi_semaphore_t           ready;          /* One post per queued event */
    uint32_t                    waiters;        /* Threads in libdmview_wait_event() */
    bool                        released;       /* Out of its display, freed by the last waiter */
} claim_t;

struct libdmview_display
{
    uint32_t                    magic;
    struct libdmview_display*   next;
    char*                       name;
    uint32_t                    generation;
    claim_t**                   claims;     /* Oldest first */
    uint32_t                    count;
    uint32_t                    capacity;
};

static dmosi_mutex_t                g_lock;
static struct libdmview_display*    g_displays;     /* Newest first */
static uint32_t                     g_next_claim = 1;

int claims_init(void)
{
    g_lock = dmosi_mutex_create(false);
    return (g_lock != NULL) ? 0 : -ENOMEM;
}

void claims_deinit(void)
{
    while (g_displays != NULL)
        libdmview_display_unregister(g_displays);
    if (g_lock != NULL)
        dmosi_mutex_destroy(g_lock);
    g_lock = NULL;
}

static bool is_display(libdmview_display_t d)
{
    return d != NULL && d->magic == DISPLAY_MAGIC;
}

static dmosi_process_id_t current_pid(void)
{
    dmosi_process_t process = dmosi_process_current();
    return (process != NULL) ? dmosi_process_get_id(process) : 0;
}

/* ---- Claims ---- */

static void claim_free(claim_t* c)
{
    for (uint32_t i = 0; i < c->prop_count; i++)
    {
        Dmod_Free(c->props[i].name);
        if (c->props[i].sval != NULL)
            Dmod_Free(c->props[i].sval);
    }
    if (c->props != NULL)
        Dmod_Free(c->props);
    if (c->ready != NULL)
        dmosi_semaphore_destroy(c->ready);
    if (c->path != NULL)
        Dmod_Free(c->path);
    Dmod_Free(c);
}

/* Out of its display; freed now, or by the last thread waiting for its events. Under g_lock */
static void claim_release(claim_t* c)
{
    if (c->waiters > 0)
    {
        c->released = true;
        (void)dmosi_semaphore_post(c->ready, 1);   /* (room: one more than the queue) */
    }
    else
        claim_free(c);
}

static void drop_claim(struct libdmview_display* d, uint32_t i)
{
    claim_t* c = d->claims[i];
    memmove(&d->claims[i], &d->claims[i + 1], (d->count - i - 1U) * sizeof(claim_t*));
    d->count--;
    d->generation++;
    claim_release(c);
}

/* The claim with this id; under g_lock */
static claim_t* find_claim(uint32_t id)
{
    for (struct libdmview_display* d = g_displays; d != NULL; d = d->next)
    {
        for (uint32_t i = 0; i < d->count; i++)
        {
            if (d->claims[i]->id == id)
                return d->claims[i];
        }
    }
    return NULL;
}

static prop_t* find_prop(claim_t* c, const char* name)
{
    for (uint32_t i = 0; c != NULL && name != NULL && i < c->prop_count; i++)
    {
        if (strcmp(c->props[i].name, name) == 0)
            return &c->props[i];
    }
    return NULL;
}

/* ---- The properties of a view file ---- */

static int file_read(void* ctx, uint32_t offset, void* buffer, size_t size)
{
    if (Dmod_FileSeek(ctx, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) != 0)
        return -EIO;
    return (Dmod_FileRead(buffer, 1, size, ctx) == size) ? 0 : -EIO;
}

static bool read_at(void* f, uint32_t offset, void* buffer, size_t size)
{
    return file_read(f, offset, buffer, size) == 0;
}

/* String `index` of the table at `table` (a valid view: it is terminated); allocated */
static char* read_string(void* f, uint32_t table, uint32_t index)
{
    uint8_t w[4];
    if (!read_at(f, table + index * 4U, w, sizeof(w)))
        return NULL;
    uint32_t at = table + rd32(w);
    size_t len = 0, size = 32;
    char* s = Dmod_Malloc(size);
    while (s != NULL)
    {
        if (len + 1U >= size)
        {
            char* bigger = Dmod_Realloc(s, size * 2U);
            if (bigger == NULL)
                break;
            s = bigger;
            size *= 2U;
        }
        if (!read_at(f, at + (uint32_t)len, s + len, 1))
            break;
        if (s[len] == '\0')
            return s;
        len++;
    }
    if (s != NULL)
        Dmod_Free(s);
    return NULL;
}

/* The `.prop` variables of the view at `path` - none when it cannot be read or is not valid */
static void load_props(claim_t* c, const char* path)
{
    size_t size = 0;
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL || !Dmod_FileSizeToSizeT(Dmod_FileSize(f), &size) || size > 0xFFFFFFFFu)
    {
        if (f != NULL)
            Dmod_FileClose(f);
        return;
    }
    dmv_input_t in = { file_read, f, (uint32_t)size };
    uint8_t h[DMV_HEADER_SIZE_0_1];
    if (dmv_validate(&in, NULL) != DMV_VALID || !read_at(f, 0, h, sizeof(h)) || rd16(h + 6) < 6)
    {
        Dmod_FileClose(f);
        return;
    }
    uint32_t strings = rd32(h + 32), vars = rd32(h + 40), var_count = rd32(h + 44);
    for (uint32_t pass = 0; pass < 2; pass++)
    {
        /* Counted first, then read */
        uint32_t n = 0;
        for (uint32_t i = 0; i < var_count; i++)
        {
            uint8_t p[sizeof(dmv_var_t)];
            if (!read_at(f, vars + i * (uint32_t)sizeof(dmv_var_t), p, sizeof(p)) || (p[1] & DMV_VARF_PROP) == 0)
                continue;
            if (pass == 1)
            {
                prop_t* prop = &c->props[n];
                prop->type = p[0];
                prop->capacity = rd16(p + 2);
                prop->name = read_string(f, strings, rd16(p + 6));
                if (prop->type == DMV_VAR_STR)
                {
                    char* init = read_string(f, strings, rd32(p + 8));
                    prop->sval = Dmod_Malloc((size_t)prop->capacity + 1U);
                    if (prop->sval != NULL)
                    {
                        strncpy(prop->sval, (init != NULL) ? init : "", prop->capacity);
                        prop->sval[prop->capacity] = '\0';
                    }
                    if (init != NULL)
                        Dmod_Free(init);
                }
                else
                    prop->ival = (int32_t)rd32(p + 8);
                if (prop->name == NULL || (prop->type == DMV_VAR_STR && prop->sval == NULL))
                {
                    if (prop->name != NULL)
                        Dmod_Free(prop->name);
                    if (prop->sval != NULL)
                        Dmod_Free(prop->sval);
                    memset(prop, 0, sizeof(*prop));
                    continue;
                }
            }
            n++;
        }
        if (pass == 0)
        {
            if (n == 0 || (c->props = Dmod_Malloc(n * sizeof(prop_t))) == NULL)
                break;
            memset(c->props, 0, n * sizeof(prop_t));
        }
        else
            c->prop_count = n;
    }
    Dmod_FileClose(f);
}

/* ---- Displays ---- */

dmod_libdmview_api_declaration(1.0, libdmview_display_t, _display_register, ( const char* name ))
{
    if (name == NULL || g_lock == NULL)
        return NULL;
    struct libdmview_display* d = Dmod_Malloc(sizeof(*d));
    char* copy = Dmod_StrDup(name);
    if (d == NULL || copy == NULL)
    {
        if (d != NULL)
            Dmod_Free(d);
        if (copy != NULL)
            Dmod_Free(copy);
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->magic = DISPLAY_MAGIC;
    d->name = copy;
    d->generation = 1;

    dmosi_mutex_lock(g_lock);
    for (struct libdmview_display* o = g_displays; o != NULL; o = o->next)
    {
        if (strcmp(o->name, name) == 0)
        {
            dmosi_mutex_unlock(g_lock);
            Dmod_Free(copy);
            Dmod_Free(d);
            return NULL;
        }
    }
    d->next = g_displays;
    g_displays = d;
    dmosi_mutex_unlock(g_lock);
    return d;
}

dmod_libdmview_api_declaration(1.0, void, _display_unregister, ( libdmview_display_t display ))
{
    if (!is_display(display) || g_lock == NULL)
        return;
    dmosi_mutex_lock(g_lock);
    for (struct libdmview_display** p = &g_displays; *p != NULL; p = &(*p)->next)
    {
        if (*p == display)
        {
            *p = display->next;
            break;
        }
    }
    while (display->count > 0)
        drop_claim(display, display->count - 1U);
    dmosi_mutex_unlock(g_lock);

    if (display->claims != NULL)
        Dmod_Free(display->claims);
    Dmod_Free(display->name);
    display->magic = 0;
    Dmod_Free(display);
}

dmod_libdmview_api_declaration(1.0, uint32_t, _display_generation, ( libdmview_display_t display ))
{
    return is_display(display) ? display->generation : 0;
}

/* Claims of processes that are gone; under g_lock */
static void drop_dead(struct libdmview_display* d)
{
    for (uint32_t i = d->count; i > 0; i--)
    {
        if (dmosi_process_find_by_id(d->claims[i - 1U]->pid) == NULL)
            drop_claim(d, i - 1U);
    }
}

dmod_libdmview_api_declaration(1.0, int, _display_view, ( libdmview_display_t display, uint32_t* generation, char* path, size_t size, size_t* length ))
{
    if (!is_display(display))
        return -EINVAL;

    dmosi_mutex_lock(g_lock);
    drop_dead(display);
    if (generation != NULL)
        *generation = display->generation;

    int ret = -ENOENT;
    if (display->count > 0)
    {
        const char* top = display->claims[display->count - 1U]->path;
        size_t len = strlen(top);
        if (length != NULL)
            *length = len;
        ret = -ERANGE;
        if (path != NULL && size > len)
        {
            memcpy(path, top, len + 1U);
            ret = 0;
        }
    }
    else if (length != NULL)
        *length = 0;
    dmosi_mutex_unlock(g_lock);
    return ret;
}

dmod_libdmview_api_declaration(1.0, int, _claim, ( const char* display, const char* view_path, uint32_t* claim ))
{
    if (view_path == NULL || claim == NULL || g_lock == NULL)
        return -EINVAL;
    claim_t* c = Dmod_Malloc(sizeof(*c));
    if (c == NULL)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));
    c->path = Dmod_StrDup(view_path);
    c->ready = dmosi_semaphore_create(0, LIBDMVIEW_EVENT_QUEUE + 1U);
    c->pid = current_pid();
    if (c->path == NULL || c->ready == NULL)
    {
        claim_free(c);
        return -ENOMEM;
    }
    load_props(c, view_path);       /* (outside the lock: it reads the file) */

    dmosi_mutex_lock(g_lock);
    /* NULL: the first registered display - the last one in the list */
    struct libdmview_display* d = NULL;
    for (struct libdmview_display* o = g_displays; o != NULL; o = o->next)
    {
        if (display == NULL || strcmp(o->name, display) == 0)
            d = o;
        if (display != NULL && d != NULL)
            break;
    }
    int ret = (d != NULL) ? 0 : -ENODEV;
    if (ret == 0 && d->count == d->capacity)
    {
        uint32_t capacity = d->capacity ? d->capacity * 2U : 4U;
        claim_t** p = Dmod_Realloc(d->claims, capacity * sizeof(claim_t*));
        if (p == NULL)
            ret = -ENOMEM;
        else
        {
            d->claims = p;
            d->capacity = capacity;
        }
    }
    if (ret == 0)
    {
        c->id = g_next_claim++;
        d->claims[d->count++] = c;
        d->generation++;
        *claim = c->id;
    }
    dmosi_mutex_unlock(g_lock);
    if (ret != 0)
        claim_free(c);
    return ret;
}

dmod_libdmview_api_declaration(1.0, int, _release, ( uint32_t claim ))
{
    if (g_lock == NULL)
        return -ENOENT;
    int ret = -ENOENT;
    dmosi_mutex_lock(g_lock);
    for (struct libdmview_display* d = g_displays; d != NULL && ret != 0; d = d->next)
    {
        for (uint32_t i = 0; i < d->count; i++)
        {
            if (d->claims[i]->id == claim)
            {
                drop_claim(d, i);
                ret = 0;
                break;
            }
        }
    }
    dmosi_mutex_unlock(g_lock);
    return ret;
}

/* ---- The channel: the application's side ---- */

static int prop_set(uint32_t claim, const char* name, uint8_t type, int32_t ival, const char* sval)
{
    if (name == NULL || (type == DMV_VAR_STR && sval == NULL) || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    claim_t* c = find_claim(claim);
    prop_t* p = find_prop(c, name);
    int ret = (p == NULL) ? -ENOENT : (p->type != type) ? -EINVAL : 0;
    if (ret == 0)
    {
        if (type == DMV_VAR_STR)
        {
            strncpy(p->sval, sval, p->capacity);
            p->sval[p->capacity] = '\0';
        }
        else
            p->ival = ival;
        p->pending = true;
        c->pending = true;
    }
    dmosi_mutex_unlock(g_lock);
    return ret;
}

dmod_libdmview_api_declaration(1.0, int, _prop_set_int, ( uint32_t claim, const char* name, int32_t value ))
{
    return prop_set(claim, name, DMV_VAR_INT, value, NULL);
}

dmod_libdmview_api_declaration(1.0, int, _prop_set_string, ( uint32_t claim, const char* name, const char* value ))
{
    return prop_set(claim, name, DMV_VAR_STR, 0, value);
}

dmod_libdmview_api_declaration(1.0, int, _prop_get_int, ( uint32_t claim, const char* name, int32_t* value ))
{
    if (value == NULL || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    prop_t* p = find_prop(find_claim(claim), name);
    int ret = (p == NULL) ? -ENOENT : (p->type != DMV_VAR_INT) ? -EINVAL : 0;
    if (ret == 0)
        *value = p->ival;
    dmosi_mutex_unlock(g_lock);
    return ret;
}

dmod_libdmview_api_declaration(1.0, int, _prop_get_string, ( uint32_t claim, const char* name, char* value, size_t size ))
{
    if (value == NULL || size == 0 || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    prop_t* p = find_prop(find_claim(claim), name);
    int ret = (p == NULL) ? -ENOENT : (p->type != DMV_VAR_STR) ? -EINVAL : 0;
    if (ret == 0)
    {
        size_t len = strlen(p->sval);
        if (len >= size)
        {
            len = size - 1U;
            ret = -ERANGE;
        }
        memcpy(value, p->sval, len);
        value[len] = '\0';
    }
    dmosi_mutex_unlock(g_lock);
    return ret;
}

dmod_libdmview_api_declaration(1.0, int, _wait_event, ( uint32_t claim, libdmview_event_t* event, int32_t timeout_ms ))
{
    if (event == NULL || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    claim_t* c = find_claim(claim);
    if (c != NULL)
        c->waiters++;
    dmosi_mutex_unlock(g_lock);
    if (c == NULL)
        return -ENOENT;

    int ret = -ETIMEDOUT;
    if (dmosi_semaphore_wait(c->ready, 1, timeout_ms) == 0)
    {
        dmosi_mutex_lock(g_lock);
        if (c->released)
            ret = -ENOENT;
        else if (c->count > 0)
        {
            *event = c->events[c->first];
            event->dropped = c->dropped;
            c->dropped = 0;
            c->first = (c->first + 1U) % LIBDMVIEW_EVENT_QUEUE;
            c->count--;
            ret = 0;
        }
        dmosi_mutex_unlock(g_lock);
    }

    dmosi_mutex_lock(g_lock);
    c->waiters--;
    bool last = c->released && c->waiters == 0;
    if (c->released && c->waiters > 0)
        (void)dmosi_semaphore_post(c->ready, 1);   /* The next waiter learns it too */
    dmosi_mutex_unlock(g_lock);
    if (last)
        claim_free(c);
    return ret;
}

/* ---- The channel: the view's side (the service's thread) ---- */

dmod_libdmview_api_declaration(1.0, int, _attach, ( libdmview_t view, libdmview_display_t display ))
{
    if (view == NULL || view->magic != VIEW_MAGIC || !is_display(display))
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    claim_t* c = (display->count > 0) ? display->claims[display->count - 1U] : NULL;
    view->claim = (c != NULL) ? c->id : 0;
    /* What the view holds is what the application reads - unless it set it meanwhile */
    for (uint32_t i = 0; c != NULL && i < view->var_count; i++)
    {
        const dmv_var_t* var = &view->vars[i];
        prop_t* p = ((var->flags & DMV_VARF_PROP) != 0) ? find_prop(c, view_string(view, var->env)) : NULL;
        if (p == NULL || p->pending || p->type != var->type)
            continue;
        if (p->type == DMV_VAR_STR)
        {
            strncpy(p->sval, view->strs[i], p->capacity);
            p->sval[p->capacity] = '\0';
        }
        else
            p->ival = view->ints[i];
    }
    dmosi_mutex_unlock(g_lock);
    return (c != NULL) ? 0 : -ENOENT;
}

/* A property the view changed itself */
void claims_publish(struct libdmview* v, uint32_t index)
{
    if (v->claim == 0 || g_lock == NULL)
        return;
    const dmv_var_t* var = &v->vars[index];
    dmosi_mutex_lock(g_lock);
    prop_t* p = find_prop(find_claim(v->claim), view_string(v, var->env));
    if (p != NULL && !p->pending && p->type == var->type)
    {
        if (p->type == DMV_VAR_STR)
        {
            strncpy(p->sval, v->strs[index], p->capacity);
            p->sval[p->capacity] = '\0';
        }
        else
            p->ival = v->ints[index];
    }
    dmosi_mutex_unlock(g_lock);
}

/* EMIT */
void claims_emit(struct libdmview* v, const char* name, int32_t value)
{
    if (v->claim == 0 || g_lock == NULL)
        return;
    dmosi_mutex_lock(g_lock);
    claim_t* c = find_claim(v->claim);
    if (c != NULL)
    {
        bool full = c->count == LIBDMVIEW_EVENT_QUEUE;
        if (full)
        {
            /* The oldest goes - its post stays for the new one */
            c->first = (c->first + 1U) % LIBDMVIEW_EVENT_QUEUE;
            c->count--;
            c->dropped++;
        }
        libdmview_event_t* e = &c->events[(c->first + c->count) % LIBDMVIEW_EVENT_QUEUE];
        strncpy(e->name, name, sizeof(e->name) - 1U);
        e->name[sizeof(e->name) - 1U] = '\0';
        e->value = value;
        e->dropped = 0;
        c->count++;
        if (!full)
            (void)dmosi_semaphore_post(c->ready, 1);
    }
    dmosi_mutex_unlock(g_lock);
}

/* What the application set, into the view, with the view's .change handlers */
void claims_apply(struct libdmview* v)
{
    if (v->claim == 0 || g_lock == NULL)
        return;
    for (;;)
    {
        /* One property at a time, out of the lock: the handlers it runs may publish and emit */
        dmosi_mutex_lock(g_lock);
        claim_t* c = find_claim(v->claim);
        prop_t* p = NULL;
        for (uint32_t i = 0; c != NULL && c->pending && i < c->prop_count && p == NULL; i++)
        {
            if (c->props[i].pending)
                p = &c->props[i];
        }
        if (c != NULL && p == NULL)
            c->pending = false;
        int32_t ival = 0;
        char* sval = NULL;
        char* name = NULL;
        uint8_t type = 0;
        if (p != NULL)
        {
            p->pending = false;
            type = p->type;
            ival = p->ival;
            name = Dmod_StrDup(p->name);
            if (type == DMV_VAR_STR)
                sval = Dmod_StrDup(p->sval);
        }
        dmosi_mutex_unlock(g_lock);
        if (p == NULL)
            return;
        if (name == NULL || (type == DMV_VAR_STR && sval == NULL))
        {
            if (name != NULL)
                Dmod_Free(name);
            if (sval != NULL)
                Dmod_Free(sval);
            return;
        }

        for (uint32_t i = 0; i < v->var_count; i++)
        {
            const dmv_var_t* var = &v->vars[i];
            if ((var->flags & DMV_VARF_PROP) == 0 || var->type != type ||
                strcmp(view_string(v, var->env), name) != 0)
                continue;
            if (type == DMV_VAR_STR)
                view_set_string(v, i, sval);
            else
                view_set_int(v, i, ival);
            for (uint32_t k = 0; k < v->item_count; k++)
            {
                if (v->items[k].kind == DMV_ITEM_CHANGE && v->items[k].value == i)
                    exec_handler(v, ROOT, v->items[k].label);
            }
        }
        Dmod_Free(name);
        if (sval != NULL)
            Dmod_Free(sval);
    }
}
