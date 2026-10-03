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
 */

#define DISPLAY_MAGIC   0x44535059u     /* 'DSPY' */

typedef struct
{
    uint32_t            id;
    dmosi_process_id_t  pid;
    char*               path;
} claim_t;

struct libdmview_display
{
    uint32_t                    magic;
    struct libdmview_display*   next;
    char*                       name;
    uint32_t                    generation;
    claim_t*                    claims;     /* Oldest first */
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

static void drop_claim(struct libdmview_display* d, uint32_t i)
{
    Dmod_Free(d->claims[i].path);
    memmove(&d->claims[i], &d->claims[i + 1], (d->count - i - 1U) * sizeof(claim_t));
    d->count--;
    d->generation++;
}

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
    dmosi_mutex_unlock(g_lock);

    while (display->count > 0)
        drop_claim(display, display->count - 1U);
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

dmod_libdmview_api_declaration(1.0, int, _display_view, ( libdmview_display_t display, uint32_t* generation, char* path, size_t size, size_t* length ))
{
    if (!is_display(display))
        return -EINVAL;

    dmosi_mutex_lock(g_lock);
    /* Claims of processes that are gone */
    for (uint32_t i = display->count; i > 0; i--)
    {
        if (dmosi_process_find_by_id(display->claims[i - 1U].pid) == NULL)
            drop_claim(display, i - 1U);
    }
    if (generation != NULL)
        *generation = display->generation;

    int ret = -ENOENT;
    if (display->count > 0)
    {
        const char* top = display->claims[display->count - 1U].path;
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
    char* copy = Dmod_StrDup(view_path);
    if (copy == NULL)
        return -ENOMEM;
    dmosi_process_id_t pid = current_pid();

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
        claim_t* p = Dmod_Realloc(d->claims, capacity * sizeof(claim_t));
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
        claim_t* c = &d->claims[d->count++];
        c->id = g_next_claim++;
        c->pid = pid;
        c->path = copy;
        d->generation++;
        *claim = c->id;
    }
    dmosi_mutex_unlock(g_lock);
    if (ret != 0)
        Dmod_Free(copy);
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
            if (d->claims[i].id == claim)
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
