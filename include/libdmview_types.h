#ifndef LIBDMVIEW_TYPES_H
#define LIBDMVIEW_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmview_format.h"

/** A loaded view (opaque). */
typedef struct libdmview* libdmview_t;

/** A display registered by its service (opaque). */
typedef struct libdmview_display* libdmview_display_t;

/** The views rented out on a display, for its service to compose (opaque, libdmview_rents_create()). */
typedef struct libdmview_rents* libdmview_rents_t;

/** Where a view draws: a framebuffer in memory. */
typedef struct
{
    void*       pixels;     /**< Top-left pixel */
    uint16_t    width;      /**< Pixels per line */
    uint16_t    height;     /**< Lines */
    uint32_t    stride;     /**< Bytes per line */
    uint8_t     format;     /**< dmdrvi_gfx_pixel_format_t - RGB565 and ARGB8888 are supported */
} libdmview_surface_t;

/** A rectangle in surface coordinates; empty when w or h is 0. */
typedef struct
{
    int16_t     x;
    int16_t     y;
    uint16_t    w;
    uint16_t    h;
} libdmview_rect_t;

/** Longest name of an event (EMIT) with its terminating zero. */
#define LIBDMVIEW_EVENT_NAME_MAX    32u

/** Events of a claim kept until the application takes them (the oldest are dropped). */
#define LIBDMVIEW_EVENT_QUEUE       16u

/** An event the view of a claim emitted (EMIT name, value). */
typedef struct
{
    char        name[LIBDMVIEW_EVENT_NAME_MAX];
    int32_t     value;
    uint32_t    dropped;    /**< Events dropped before this one - the application was too slow */
} libdmview_event_t;

/** Longest path of a view in libdmview_rent_info_t, with the terminating zero. */
#define LIBDMVIEW_PATH_MAX      192u

/** Most rents of one claim the service composes (libdmview_display_rents()). */
#define LIBDMVIEW_RENTS_MAX     8u

/**
 * A rent as the service of a display sees it: a rectangle of the view of the claim on top of the display, and the view of the
 * application that took it.
 */
typedef struct
{
    uint32_t            id;
    uint32_t            claim;      /**< The claim of the application in the rent, 0: nobody has taken it */
    libdmview_rect_t    rect;       /**< In the coordinates of the view of the owner's claim */
    bool                visible;
    bool                focus;      /**< The keys go to it */
    uint16_t            escape;     /**< The key (USB HID usage) that goes to the owner, 0: none */
    char                path[LIBDMVIEW_PATH_MAX];   /**< The view of that claim, "" without one */
} libdmview_rent_info_t;

/** libdmview_update() result when nothing is pending. */
#define LIBDMVIEW_NO_DEADLINE   0xFFFFFFFFu

#endif /* LIBDMVIEW_TYPES_H */
