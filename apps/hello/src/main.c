#include "dmod.h"
#include "dmosi.h"
#include "libdmview.h"
#include <errno.h>
#include <string.h>

/**
 * @brief hello - a small application that lives in a rent of the screen.
 *
 * Usage: hello [--display NAME] [<view>]
 *
 * NAME is the display it takes, or the rent of one that its owner gave it (libdmview_rent_create(): "<display>/<rent>", which the
 * launcher passes to every application it starts); without it, the first display. Its view (views/hello.dmvs) draws into what it is
 * given, and it shows the size of that, the taps on its button and the keys typed while it has the focus: the three properties of the
 * view, set from the events of the claim ("tap", "char", "rent.size"). It ends when its claim is gone - the owner took the rent back.
 */

#define DEFAULT_VIEW    "/eviews/hello/hello.dmv"   /* dmod-boot: views of the flash modules */
#define WAIT_MS         100u
#define DISPLAY_WAIT_MS 10000u
#define TYPED_MAX       16

static void show_size(uint32_t claim, uint16_t w, uint16_t h)
{
    char text[24];
    Dmod_SnPrintf(text, sizeof(text), "%u x %u", (unsigned)w, (unsigned)h);
    (void)libdmview_prop_set_string(claim, "size", text);
}

int main(int argc, char* argv[])
{
    const char* display = NULL;
    const char* view = DEFAULT_VIEW;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc)
            display = argv[++i];
        else if (argv[i][0] != '\0')
            view = argv[i];
    }

    /* The rent (or the display) may be there a moment after this starts */
    dmosi_semaphore_t wakeup = dmosi_semaphore_create(0, 1);
    if (wakeup == NULL)
        return -ENOMEM;
    uint32_t claim = 0;
    int ret = -ENODEV;
    for (uint32_t waited = 0; waited <= DISPLAY_WAIT_MS; waited += WAIT_MS)
    {
        ret = libdmview_claim(display, view, &claim);
        if (ret != -ENODEV)
            break;
        (void)dmosi_semaphore_wait(wakeup, 1, (int32_t)WAIT_MS);
    }
    if (ret != 0)
    {
        DMOD_LOG_ERROR("hello: cannot claim %s (%d)\n", (display != NULL) ? display : "a display", ret);
        dmosi_semaphore_destroy(wakeup);
        return ret;
    }

    uint16_t w = 0, h = 0;
    if (libdmview_rent_get_size(claim, &w, &h) == 0)
        show_size(claim, w, h);
    else
        (void)libdmview_prop_set_string(claim, "size", "the whole display");
    DMOD_LOG_INFO("hello: showing %s on %s\n", view, (display != NULL) ? display : "the display");

    uint32_t taps = 0;
    char typed[TYPED_MAX + 1] = "";
    libdmview_event_t ev;
    while ((ret = libdmview_wait_event(claim, &ev, -1)) != -ENOENT)
    {
        if (ret != 0)
            continue;
        if (strcmp(ev.name, "tap") == 0)
        {
            char text[24];
            Dmod_SnPrintf(text, sizeof(text), "taps: %u", (unsigned)++taps);
            (void)libdmview_prop_set_string(claim, "taps", text);
        }
        else if (strcmp(ev.name, "rent.size") == 0)
        {
            show_size(claim, (uint16_t)((uint32_t)ev.value >> 16), (uint16_t)((uint32_t)ev.value & 0xFFFFu));
        }
        else if (strcmp(ev.name, "char") == 0 && ev.value >= 0x20 && ev.value < 0x7F)
        {
            size_t n = strlen(typed);
            if (n >= TYPED_MAX)                         /* the oldest character goes */
            {
                memmove(typed, typed + 1, TYPED_MAX - 1);
                n = TYPED_MAX - 1;
            }
            typed[n] = (char)ev.value;
            typed[n + 1] = '\0';
            (void)libdmview_prop_set_string(claim, "typed", typed);
        }
        else if (strcmp(ev.name, "key") == 0 && ev.value == 0x2A)    /* Backspace */
        {
            size_t n = strlen(typed);
            if (n > 0)
                typed[n - 1] = '\0';
            (void)libdmview_prop_set_string(claim, "typed", typed);
        }
    }
    DMOD_LOG_INFO("hello: its claim is gone - ends\n");
    dmosi_semaphore_destroy(wakeup);
    return 0;
}
