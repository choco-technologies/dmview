#include "dmod.h"
#include "dmosi.h"
#include "libdmview.h"
#include "libsystemd.h"
#include "dmvfs.h"
#include <errno.h>
#include <string.h>

/**
 * @brief dmview - the display service: shows a view on one display.
 *
 * Started per display by the device rule `[class=display] start=dmview@%name`
 * (configs/dmview.rules, dmdevfs reports every node answering
 * DMDRVI_IOCTL_GFX_GET_INFO) with the display's name and node path.
 *
 * The view shown is, in this order:
 *  1. the newest live claim of an application (libdmview_claim()),
 *  2. $DMVIEW_VIEWS/<display name>.dmv, if that file exists,
 *  3. $DMVIEW_DEFAULT.
 *
 * Input comes from the member of the display's friends_group that is an
 * input device with contacts of the display's resolution.
 *
 * A view designed for a portrait screen on a landscape display (or the
 * other way round) is turned by 90 degrees: it is drawn into a buffer of
 * its own orientation, and what changed is copied turned onto the screen;
 * touches are turned back. $DMVIEW_ROTATION (0, 90, 180, 270 - clockwise)
 * turns every view by that angle instead.
 */

#define CLAIM_POLL_MS   100u     /* Longest wait - claims and stop requests are seen this fast */
#define ROTATE_TILE     16u      /* Lines of a view turned at once - they stay in the cache */

typedef struct
{
    const char*         name;
    void*               gfx;
    dmdrvi_gfx_info_t   info;
    libdmview_surface_t screen;     /* The display's framebuffer */
    libdmview_surface_t surface;    /* What the view draws into: the screen, or `turned` */
    uint16_t            rotation;   /* Clockwise degrees the view is turned by on the screen */
    void*               turned;     /* Buffer of a turned view */
    void*               touch;
    libdmview_display_t display;
    uint32_t            generation;
    libdmview_t         view;
    char*               view_path;
    dmosi_semaphore_t   wakeup;
    bool                present;    /* The driver has DMDRVI_IOCTL_GFX_PRESENT */
} service_t;

static char* concat(const char* a, const char* b, const char* c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char* s = Dmod_Malloc(la + lb + lc + 1U);
    if (s != NULL)
    {
        memcpy(s, a, la);
        memcpy(s + la, b, lb);
        memcpy(s + la + lb, c, lc + 1U);
    }
    return s;
}

/* ---- Devices ---- */

/*
 * Make `area` (NULL: everything) of what was drawn visible. With
 * DMDRVI_IOCTL_GFX_PRESENT a double buffered display shows the finished
 * frame at the next vertical blank - nothing is ever drawn on the screen -
 * and the drawing buffer changes: the surface follows it. A driver without
 * it gets the drawing flushed, as before.
 */
static void present(service_t* s, const dmdrvi_gfx_rect_t* area)
{
    if (s->present)
    {
        int ret = Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_GFX_PRESENT, (void*)area);
        if (ret == 0)
        {
            void* pixels = NULL;
            if (s->info.buffer_count > 1 && Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER, &pixels) == 0 &&
                pixels != NULL)
            {
                s->screen.pixels = pixels;
                if (s->turned == NULL)
                    s->surface.pixels = pixels;
            }
            return;
        }
        if (ret != -ENOTTY)
        {
            DMOD_LOG_WARN("dmview: presenting on %s failed (%d)\n", s->name, ret);
            return;
        }
        s->present = false;
    }
    (void)dmvfs_fflush(s->gfx);         /* The driver makes the drawing visible (data cache) */
}

static bool open_display(service_t* s, const char* path)
{
    void* pixels = NULL;
    s->gfx = Dmod_FileOpen(path, "r+");
    if (s->gfx == NULL)
    {
        DMOD_LOG_ERROR("dmview: cannot open display %s\n", path);
        return false;
    }
    if (Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_GFX_GET_INFO, &s->info) != 0 ||
        Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER, &pixels) != 0 || pixels == NULL)
    {
        DMOD_LOG_ERROR("dmview: %s is not a usable display\n", path);
        return false;
    }
    s->screen.pixels = pixels;
    s->screen.width = s->info.width;
    s->screen.height = s->info.height;
    s->screen.stride = s->info.stride;
    s->screen.format = (uint8_t)s->info.pixel_format;
    s->surface = s->screen;
    s->present = true;
    if (s->info.buffer_count > 1)
        DMOD_LOG_INFO("dmview: %s is double buffered\n", path);
    return true;
}

/* The friend of the display that is an input device of its resolution */
static void open_touch(service_t* s, const char* display_path)
{
    for (uint32_t index = 0; ; index++)
    {
        dmdrvi_devfs_friend_t f;
        memset(&f, 0, sizeof(f));
        f.index = index;
        if (Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_DEVFS_GET_FRIEND, &f) != -ERANGE)   /* NULL buffers: lengths */
            break;
        f.path_size = f.path_length + 1U;
        f.role_size = f.role_length + 1U;
        f.path = Dmod_Malloc(f.path_size);
        f.role = Dmod_Malloc(f.role_size);
        void* input = NULL;
        if (f.path != NULL && f.role != NULL && Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_DEVFS_GET_FRIEND, &f) == 0)
            input = Dmod_FileOpen(f.path, "r");

        dmdrvi_input_info_t info;
        if (input != NULL && Dmod_Ioctl(input, DMDRVI_IOCTL_INPUT_GET_INFO, &info) == 0 &&
            (info.capabilities & DMDRVI_INPUT_CAP_CONTACTS) != 0)
        {
            if (info.width == s->info.width && info.height == s->info.height)
            {
                DMOD_LOG_INFO("dmview: %s takes its input from %s\n", display_path, f.path);
                s->touch = input;
                input = NULL;
            }
            else
            {
                DMOD_LOG_WARN("dmview: %s is %ux%u, the display %ux%u - not used\n", f.path,
                              (unsigned)info.width, (unsigned)info.height,
                              (unsigned)s->info.width, (unsigned)s->info.height);
            }
        }
        if (input != NULL)
            Dmod_FileClose(input);
        if (f.path != NULL)
            Dmod_Free(f.path);
        if (f.role != NULL)
            Dmod_Free(f.role);
        if (s->touch != NULL)
            return;
    }
    DMOD_LOG_INFO("dmview: %s has no touch input\n", display_path);
}

/* ---- Rotation ---- */

static uint32_t bytes_per_pixel(uint8_t format)
{
    return (format == DMDRVI_GFX_PIXEL_FORMAT_RGB565) ? 2U : 4U;      /* What libdmview draws */
}

/* The angle to turn a view of w x h by: $DMVIEW_ROTATION, else 90 when it
 * fits the screen only turned */
static uint16_t rotation_for(const service_t* s, uint16_t w, uint16_t h)
{
    const char* env = Dmod_GetEnv("DMVIEW_ROTATION");
    if (env != NULL && env[0] != '\0')
    {
        uint32_t angle = 0;
        for (const char* p = env; *p >= '0' && *p <= '9'; p++)
            angle = angle * 10U + (uint32_t)(*p - '0');
        if (angle == 0 || angle == 90 || angle == 180 || angle == 270)
            return (uint16_t)angle;
        DMOD_LOG_WARN("dmview: DMVIEW_ROTATION=%s is not 0, 90, 180 or 270 - ignored\n", env);
    }
    bool fits = w <= s->screen.width && h <= s->screen.height;
    bool fits_turned = w <= s->screen.height && h <= s->screen.width;
    return (!fits && fits_turned) ? 90U : 0U;
}

/* Draw views turned by `rotation`: into a buffer of the turned size */
static void set_rotation(service_t* s, uint16_t rotation)
{
    if (rotation == s->rotation && (rotation == 0) == (s->turned == NULL))
        return;
    if (s->turned != NULL)
        Dmod_Free(s->turned);
    s->turned = NULL;
    s->rotation = 0;
    s->surface = s->screen;
    if (rotation == 0)
        return;

    libdmview_surface_t turned = s->screen;
    if (rotation != 180)
    {
        turned.width = s->screen.height;
        turned.height = s->screen.width;
    }
    turned.stride = turned.width * bytes_per_pixel(turned.format);
    turned.pixels = Dmod_Malloc((size_t)turned.stride * turned.height);
    if (turned.pixels == NULL)
    {
        DMOD_LOG_ERROR("dmview: no memory to turn views on %s\n", s->name);
        return;
    }
    s->turned = turned.pixels;
    s->surface = turned;
    s->rotation = rotation;
    DMOD_LOG_INFO("dmview: views on %s are turned by %u degrees\n", s->name, (unsigned)rotation);
}

/* Copy `r` of the turned buffer onto the screen; returns the screen's area */
static dmdrvi_gfx_rect_t copy_turned(service_t* s, const libdmview_rect_t* r)
{
    const uint32_t bpp = bytes_per_pixel(s->screen.format);
    const int32_t sw = s->screen.width, sh = s->screen.height, stride = (int32_t)s->screen.stride;
    uint8_t* base = s->screen.pixels;
    uint8_t* origin;                    /* The view's (0, 0) on the screen */
    int32_t step_x, step_y;             /* Bytes on the screen from one view pixel to the next, the next line */
    dmdrvi_gfx_rect_t area;
    switch (s->rotation)
    {
        case 90:
            origin = base + (sw - 1) * (int32_t)bpp;
            step_x = stride;
            step_y = -(int32_t)bpp;
            area = (dmdrvi_gfx_rect_t){ (uint16_t)(sw - (r->y + r->h)), (uint16_t)r->x, r->h, r->w };
            break;
        case 180:
            origin = base + (sh - 1) * stride + (sw - 1) * (int32_t)bpp;
            step_x = -(int32_t)bpp;
            step_y = -stride;
            area = (dmdrvi_gfx_rect_t){ (uint16_t)(sw - (r->x + r->w)), (uint16_t)(sh - (r->y + r->h)), r->w, r->h };
            break;
        default:    /* 270 */
            origin = base + (sh - 1) * stride;
            step_x = -stride;
            step_y = (int32_t)bpp;
            area = (dmdrvi_gfx_rect_t){ (uint16_t)r->y, (uint16_t)(sh - (r->x + r->w)), r->h, r->w };
            break;
    }

    /* A few lines at a time, column by column: the screen is written in runs */
    const uint8_t* src = s->surface.pixels;
    for (int32_t y0 = r->y; y0 < r->y + r->h; y0 += (int32_t)ROTATE_TILE)
    {
        int32_t y1 = y0 + (int32_t)ROTATE_TILE;
        if (y1 > r->y + r->h)
            y1 = r->y + r->h;
        for (int32_t x = r->x; x < r->x + r->w; x++)
        {
            const uint8_t* from = src + (size_t)y0 * s->surface.stride + (size_t)x * bpp;
            uint8_t* to = origin + x * step_x + y0 * step_y;
            if (bpp == 2U)
            {
                for (int32_t y = y0; y < y1; y++, from += s->surface.stride, to += step_y)
                    *(uint16_t*)to = *(const uint16_t*)from;
            }
            else
            {
                for (int32_t y = y0; y < y1; y++, from += s->surface.stride, to += step_y)
                    *(uint32_t*)to = *(const uint32_t*)from;
            }
        }
    }
    return area;
}

/* A touch on the screen, in the turned view's coordinates */
static void turn_input(const service_t* s, dmdrvi_input_state_t* state)
{
    const int32_t sw = s->screen.width, sh = s->screen.height;
    for (uint32_t i = 0; i < state->contact_count && i < DMDRVI_INPUT_MAX_CONTACTS; i++)
    {
        int32_t x = state->contacts[i].x, y = state->contacts[i].y;
        switch (s->rotation)
        {
            case 90:  state->contacts[i].x = (uint16_t)y;            state->contacts[i].y = (uint16_t)(sw - 1 - x); break;
            case 180: state->contacts[i].x = (uint16_t)(sw - 1 - x); state->contacts[i].y = (uint16_t)(sh - 1 - y); break;
            default:  state->contacts[i].x = (uint16_t)(sh - 1 - y); state->contacts[i].y = (uint16_t)x;            break;
        }
    }
    int16_t dx = state->dx, dy = state->dy;
    switch (s->rotation)
    {
        case 90:  state->dx = dy;                 state->dy = (int16_t)-dx; break;
        case 180: state->dx = (int16_t)-dx;       state->dy = (int16_t)-dy; break;
        default:  state->dx = (int16_t)-dy;       state->dy = dx;           break;
    }
}

/* ---- Views ---- */

/* Path of the view to show now, allocated; NULL when there is none */
static char* choose_view(service_t* s)
{
    size_t length = 0;
    uint32_t generation = 0;
    if (libdmview_display_view(s->display, &generation, NULL, 0, &length) == -ERANGE)
    {
        char* path = Dmod_Malloc(length + 1U);
        if (path != NULL && libdmview_display_view(s->display, &generation, path, length + 1U, &length) == 0)
        {
            s->generation = generation;
            return path;
        }
        if (path != NULL)
            Dmod_Free(path);
    }
    s->generation = generation;

    const char* dir = Dmod_GetEnv("DMVIEW_VIEWS");
    if (dir != NULL && dir[0] != '\0')
    {
        char* path = concat(dir, (dir[strlen(dir) - 1] == '/') ? "" : "/", s->name);
        char* file = (path != NULL) ? concat(path, ".dmv", "") : NULL;
        if (path != NULL)
            Dmod_Free(path);
        if (file != NULL && Dmod_FileAvailable(file))
            return file;
        if (file != NULL)
            Dmod_Free(file);
    }
    const char* fallback = Dmod_GetEnv("DMVIEW_DEFAULT");
    return (fallback != NULL && fallback[0] != '\0') ? Dmod_StrDup(fallback) : NULL;
}

static void show_view(service_t* s, char* path)
{
    if (path != NULL && s->view_path != NULL && strcmp(path, s->view_path) == 0 && s->view != NULL)
    {
        Dmod_Free(path);
        return;
    }
    libdmview_close(s->view);
    s->view = NULL;
    if (s->view_path != NULL)
        Dmod_Free(s->view_path);
    s->view_path = path;
    if (path == NULL)
    {
        DMOD_LOG_WARN("dmview: no view for %s (DMVIEW_VIEWS, DMVIEW_DEFAULT)\n", s->name);
        dmdrvi_gfx_fill_rect_t all = { 0, 0, s->info.width, s->info.height, 0xFF000000u };
        (void)Dmod_Ioctl(s->gfx, DMDRVI_IOCTL_GFX_FILL_RECT, &all);
        present(s, NULL);
        return;
    }
    int status = 0;
    s->view = libdmview_open(path, &status);
    if (s->view == NULL)
    {
        DMOD_LOG_ERROR("dmview: cannot show %s on %s (%d)\n", path, s->name, status);
    }
    else
    {
        uint16_t w = 0, h = 0;
        (void)libdmview_get_size(s->view, &w, &h);
        set_rotation(s, rotation_for(s, w, h));
        DMOD_LOG_INFO("dmview: showing %s on %s\n", path, s->name);
    }
}

/* ---- Main loop ---- */

static void run(service_t* s)
{
    uint32_t deadline = 0;              /* Ms until what the view has due, from `deadline_from` */
    uint32_t deadline_from = dmosi_get_tick_count();
    while (!libsystemd_stop_requested())
    {
        /* What is left of it - drawing took some: a late frame waits for nothing */
        uint32_t spent = dmosi_get_tick_count() - deadline_from;
        uint32_t left = (deadline == LIBDMVIEW_NO_DEADLINE) ? deadline : (spent < deadline) ? deadline - spent : 0U;
        uint32_t wait = (left < CLAIM_POLL_MS) ? left : CLAIM_POLL_MS;
        dmdrvi_input_state_t state;
        bool have_state = false;

        if (s->touch != NULL)
            have_state = Dmod_Ioctl(s->touch, DMDRVI_IOCTL_INPUT_WAIT_EVENT, &wait) == 0 &&
                         Dmod_FileRead(&state, 1, sizeof(state), s->touch) == sizeof(state);
        else
            (void)dmosi_semaphore_wait(s->wakeup, 1, (int32_t)wait);

        uint32_t now = dmosi_get_tick_count();
        if (s->view != NULL && have_state)
        {
            if (s->turned != NULL)
                turn_input(s, &state);
            (void)libdmview_input(s->view, &state, now);
        }
        deadline = (s->view != NULL) ? libdmview_update(s->view, now) : LIBDMVIEW_NO_DEADLINE;
        deadline_from = now;

        /* A view going to another view, or a claim that came or went */
        const char* next = (s->view != NULL) ? libdmview_take_goto(s->view) : NULL;
        if (next != NULL)
            show_view(s, Dmod_StrDup(next));
        if (libdmview_display_generation(s->display) != s->generation)
            show_view(s, choose_view(s));

        libdmview_rect_t changed;
        if (s->view != NULL && libdmview_render(s->view, &s->surface, &changed) > 0)
        {
            dmdrvi_gfx_rect_t area = { (uint16_t)changed.x, (uint16_t)changed.y, changed.w, changed.h };
            if (s->turned != NULL)
                area = copy_turned(s, &changed);
            present(s, &area);
        }
    }
}

int main(int argc, char* argv[])
{
    service_t s;
    memset(&s, 0, sizeof(s));

    if (argc < 2 || argv[1][0] == '\0')
    {
        Dmod_Printf("Usage: dmview <display name> [<display node>]\n");
        Dmod_Printf("Started per display from dmview@.ini - see dmview.rules\n");
        return -EINVAL;
    }
    s.name = argv[1];
    char* default_path = concat("/dev/", s.name, "");
    const char* path = (argc > 2 && argv[2][0] != '\0') ? argv[2] : default_path;

    int ret = 0;
    s.wakeup = dmosi_semaphore_create(0, 1);
    if (s.wakeup == NULL || default_path == NULL || !open_display(&s, path))
        ret = -ENODEV;
    if (ret == 0)
    {
        open_touch(&s, path);
        s.display = libdmview_display_register(s.name);
        if (s.display == NULL)
        {
            DMOD_LOG_ERROR("dmview: display %s is already served\n", s.name);
            ret = -EBUSY;
        }
    }
    if (ret == 0)
    {
        libsystemd_set_stop_semaphore(s.wakeup);
        show_view(&s, choose_view(&s));
        run(&s);
        libsystemd_set_stop_semaphore(NULL);
    }

    libdmview_close(s.view);
    if (s.turned != NULL)
        Dmod_Free(s.turned);
    if (s.view_path != NULL)
        Dmod_Free(s.view_path);
    libdmview_display_unregister(s.display);
    if (s.touch != NULL)
        Dmod_FileClose(s.touch);
    if (s.gfx != NULL)
        Dmod_FileClose(s.gfx);
    if (s.wakeup != NULL)
        dmosi_semaphore_destroy(s.wakeup);
    if (default_path != NULL)
        Dmod_Free(default_path);
    return ret;
}
