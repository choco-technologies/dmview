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

/** libdmview_update() result when nothing is pending. */
#define LIBDMVIEW_NO_DEADLINE   0xFFFFFFFFu

#endif /* LIBDMVIEW_TYPES_H */
