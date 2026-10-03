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
 */

#define CLAIM_POLL_MS   100u     /* Longest wait - claims and stop requests are seen this fast */

typedef struct
{
    const char*         name;
    void*               gfx;
    dmdrvi_gfx_info_t   info;
    libdmview_surface_t surface;
    void*               touch;
    libdmview_display_t display;
    uint32_t            generation;
    libdmview_t         view;
    char*               view_path;
    dmosi_semaphore_t   wakeup;
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
    s->surface.pixels = pixels;
    s->surface.width = s->info.width;
    s->surface.height = s->info.height;
    s->surface.stride = s->info.stride;
    s->surface.format = (uint8_t)s->info.pixel_format;
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
        DMOD_LOG_INFO("dmview: showing %s on %s\n", path, s->name);
    }
}

/* ---- Main loop ---- */

static void run(service_t* s)
{
    uint32_t deadline = 0;
    while (!libsystemd_stop_requested())
    {
        uint32_t wait = (deadline < CLAIM_POLL_MS) ? deadline : CLAIM_POLL_MS;
        dmdrvi_input_state_t state;
        bool have_state = false;

        if (s->touch != NULL)
            have_state = Dmod_Ioctl(s->touch, DMDRVI_IOCTL_INPUT_WAIT_EVENT, &wait) == 0 &&
                         Dmod_FileRead(&state, 1, sizeof(state), s->touch) == sizeof(state);
        else
            (void)dmosi_semaphore_wait(s->wakeup, 1, (int32_t)wait);

        uint32_t now = dmosi_get_tick_count();
        if (s->view != NULL && have_state)
            (void)libdmview_input(s->view, &state, now);
        deadline = (s->view != NULL) ? libdmview_update(s->view, now) : LIBDMVIEW_NO_DEADLINE;

        /* A view going to another view, or a claim that came or went */
        const char* next = (s->view != NULL) ? libdmview_take_goto(s->view) : NULL;
        if (next != NULL)
            show_view(s, Dmod_StrDup(next));
        if (libdmview_display_generation(s->display) != s->generation)
            show_view(s, choose_view(s));

        libdmview_rect_t changed;
        if (s->view != NULL && libdmview_render(s->view, &s->surface, &changed) > 0)
            (void)dmvfs_fflush(s->gfx);     /* The driver makes the drawing visible (data cache) */
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
