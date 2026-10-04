#include "dmod.h"
#include "dmosi.h"
#include "libdmview.h"
#include "libsystemd.h"
#include <errno.h>

/**
 * @brief dmview_demo - shows the dmodOS example (examples/dmodos) on a
 *        display while it runs.
 *
 * Usage: dmview_demo [<view>] [<display>]
 *
 * It claims the display for the view (libdmview_claim()): the display's
 * dmview service shows it until the program ends or is stopped, then
 * returns to its own view. A display that is not served yet (the services
 * start together) is waited for.
 */

#define DEFAULT_VIEW    "/eviews/dmview_demo/dmodos.dmv"    /* dmod-boot: views of the flash modules */
#define WAIT_MS         100u
#define DISPLAY_WAIT_MS 10000u

int main(int argc, char* argv[])
{
    const char* view = (argc > 1 && argv[1][0] != '\0') ? argv[1] : DEFAULT_VIEW;
    const char* display = (argc > 2 && argv[2][0] != '\0') ? argv[2] : NULL;

    if (!Dmod_FileAvailable(view))
    {
        Dmod_Printf("dmview_demo: no view %s\n", view);
        Dmod_Printf("Usage: dmview_demo [<view>] [<display>]\n");
        return -ENOENT;
    }

    dmosi_semaphore_t wakeup = dmosi_semaphore_create(0, 1);
    if (wakeup == NULL)
        return -ENOMEM;
    libsystemd_set_stop_semaphore(wakeup);

    uint32_t claim = 0;
    int ret = -ENODEV;
    for (uint32_t waited = 0; !libsystemd_stop_requested(); waited += WAIT_MS)
    {
        ret = libdmview_claim(display, view, &claim);
        if (ret != -ENODEV || waited >= DISPLAY_WAIT_MS)
            break;
        (void)dmosi_semaphore_wait(wakeup, 1, (int32_t)WAIT_MS);
    }
    if (ret == 0)
    {
        DMOD_LOG_INFO("dmview_demo: showing %s\n", view);
        while (!libsystemd_stop_requested())
            (void)dmosi_semaphore_wait(wakeup, 1, (int32_t)(10U * WAIT_MS));
        (void)libdmview_release(claim);
    }
    else if (ret == -ENODEV)
    {
        DMOD_LOG_ERROR("dmview_demo: no display %s\n", (display != NULL) ? display : "is served by dmview");
    }
    else
    {
        DMOD_LOG_ERROR("dmview_demo: cannot claim the display (%d)\n", ret);
    }

    libsystemd_set_stop_semaphore(NULL);
    dmosi_semaphore_destroy(wakeup);
    return ret;
}
