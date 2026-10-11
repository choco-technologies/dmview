#ifndef LIBDMVIEW_H
#define LIBDMVIEW_H

#include "dmod_types.h"
#include "libdmview_defs.h"
#include "libdmview_types.h"
#include "dmdrvi_ioctl.h"

/**
 * libdmview - executes dmview views (.dmv): draws them into a framebuffer
 * and runs their input handlers. One libdmview_t per view; nothing is
 * shared between views, so any number of them run at once (several
 * displays, several services).
 *
 * The display registry (libdmview_display_*, libdmview_claim) is the one
 * shared part: it lets an application show its own view on a display while
 * it runs, and return the display to its service afterwards.
 */

/* ---- Views ---- */

/**
 * @brief Load and validate a view file.
 *
 * The code and the tables are kept in memory (they are executed on every
 * redraw); the view is validated once here, so execution needs no checks.
 * Initial variable values are taken over from dmenv for `env:` variables,
 * then the `.init` handlers run.
 *
 * @param status Receives 0, -ENOENT (no such file), -EBADMSG (not a valid
 *               view), -EIO or -ENOMEM (may be NULL)
 * @return The view, or NULL
 */
dmod_libdmview_api(1.0, libdmview_t, _open, ( const char* path, int* status ));

/** @brief libdmview_open() from any input. */
dmod_libdmview_api(1.0, libdmview_t, _open_input, ( const dmv_input_t* input, int* status ));

/**
 * @brief Size the view was designed for (`.size`) - a service can turn a
 *        portrait view on a landscape display by it.
 * @return 0, -EINVAL
 */
dmod_libdmview_api(1.0, int, _get_size, ( libdmview_t view, uint16_t* width, uint16_t* height ));

/** @brief Release a view. Safe on NULL. */
dmod_libdmview_api(1.0, void, _close, ( libdmview_t view ));

/**
 * @brief Draw what changed since the last call (everything the first time,
 *        or after libdmview_invalidate() or a new surface size).
 *
 * @param changed Receives the bounding rectangle of what was drawn (may be NULL)
 * @return 1 when something was drawn, 0 when nothing changed, -ENOTSUP for
 *         an unsupported pixel format, -EINVAL
 */
dmod_libdmview_api(1.0, int, _render, ( libdmview_t view, const libdmview_surface_t* surface, libdmview_rect_t* changed ));

/** @brief Draw everything on the next libdmview_render(). */
dmod_libdmview_api(1.0, void, _invalidate, ( libdmview_t view ));

/**
 * @brief Feed the current state of the input device.
 *
 * Turns changes into events (PRESS, DRAG, RELEASE, CLICK, .key) and runs
 * their handlers. Coordinates are surface coordinates.
 */
dmod_libdmview_api(1.0, int, _input, ( libdmview_t view, const dmdrvi_input_state_t* state, uint32_t now_ms ));

/**
 * @brief Run what is due at @p now_ms: .timer handlers, LONG.
 *
 * @return Milliseconds until the next deadline, LIBDMVIEW_NO_DEADLINE when
 *         there is none
 */
dmod_libdmview_api(1.0, uint32_t, _update, ( libdmview_t view, uint32_t now_ms ));

/**
 * @brief The view a GOTO asked for, once (NULL when none). Valid until the
 *        next call or libdmview_close().
 */
dmod_libdmview_api(1.0, const char*, _take_goto, ( libdmview_t view ));

/** @brief Read an integer variable by name (without '$'). -ENOENT / -EINVAL. */
dmod_libdmview_api(1.0, int, _get_int, ( libdmview_t view, const char* name, int32_t* value ));

/** @brief Set an integer variable by name - redraws what depends on it. */
dmod_libdmview_api(1.0, int, _set_int, ( libdmview_t view, const char* name, int32_t value ));

/** @brief Set a string variable by name (truncated to its size) - redraws what depends on it. */
dmod_libdmview_api(1.0, int, _set_string, ( libdmview_t view, const char* name, const char* value ));

/**
 * @brief Check a view without loading it (dmview_format.h, docs/binary-format.md).
 *
 * @return DMV_VALID or the first problem, its offset in @p error_offset
 */
dmod_libdmview_api(1.0, dmv_status_t, _validate, ( const dmv_input_t* input, uint32_t* error_offset ));

/* ---- Displays and claims ---- */

/**
 * @brief Register a display (called by its service).
 *
 * @return The registration, NULL when out of memory or the name is taken
 */
dmod_libdmview_api(1.0, libdmview_display_t, _display_register, ( const char* name ));

/** @brief Unregister a display; its claims are dropped. Safe on NULL. */
dmod_libdmview_api(1.0, void, _display_unregister, ( libdmview_display_t display ));

/**
 * @brief The view an application claimed the display for (the newest live
 *        claim), for the service to show.
 *
 * Claims of processes that are gone are dropped here. @p generation changes
 * whenever the answer may have changed, so a service only has to compare it.
 *
 * @param path   Buffer for the view path (may be NULL to ask for the length)
 * @param length Receives the length of the path
 * @return 0, -ENOENT when there is no claim, -ERANGE when @p path is too small
 */
dmod_libdmview_api(1.0, int, _display_view, ( libdmview_display_t display, uint32_t* generation, char* path, size_t size, size_t* length ));

/** @brief Generation of a display - see libdmview_display_view(). */
dmod_libdmview_api(1.0, uint32_t, _display_generation, ( libdmview_display_t display ));

/**
 * @brief Show @p view_path on a display while the calling process runs.
 *
 * Claims stack: the newest one is shown; libdmview_release() - or the
 * process ending - returns the display to the previous claim, and finally to
 * the service's own view.
 *
 * @param display NULL for the first registered display
 * @param claim   Receives the claim, for libdmview_release()
 * @return 0, -ENODEV when there is no such display, -ENOMEM, -EINVAL
 */
dmod_libdmview_api(1.0, int, _claim, ( const char* display, const char* view_path, uint32_t* claim ));

/** @brief Drop a claim. -ENOENT when there is none with this id. */
dmod_libdmview_api(1.0, int, _release, ( uint32_t claim ));

/* ---- The channel of a claim ----
 *
 * A claim is also a channel between the application and its view: the
 * view's properties (`.prop $var, type, init, "name"`), which both may set
 * and read - a change made by the application runs the view's `.change`
 * handlers -, and the events the view emits (`EMIT "name", value`), which
 * the application takes in order. Names are the view's own: two
 * applications with a property "title" do not meet.
 *
 * The properties are read from the view file when it is claimed, so a
 * name or a type the view does not have is an error at once. What the
 * application sets reaches the view on the service's thread within
 * 100 ms (the service's poll) - also before the view is shown.
 */

/** @brief Set an integer property of the claim's view. -ENOENT (no claim, no such property), -EINVAL (not an integer). */
dmod_libdmview_api(1.0, int, _prop_set_int, ( uint32_t claim, const char* name, int32_t value ));

/** @brief Set a string property (cut to its size). -ENOENT, -EINVAL (not a string). */
dmod_libdmview_api(1.0, int, _prop_set_string, ( uint32_t claim, const char* name, const char* value ));

/** @brief An integer property - as the view or the application last set it. -ENOENT, -EINVAL. */
dmod_libdmview_api(1.0, int, _prop_get_int, ( uint32_t claim, const char* name, int32_t* value ));

/** @brief A string property. -ENOENT, -EINVAL, -ERANGE (@p value too small: cut). */
dmod_libdmview_api(1.0, int, _prop_get_string, ( uint32_t claim, const char* name, char* value, size_t size ));

/**
 * @brief Take the next event the claim's view emitted, waiting for one at
 *        most @p timeout_ms (0: does not wait, -1: no limit).
 *
 * @return 0, -ETIMEDOUT, -ENOENT (no such claim, or it was released while waiting)
 */
dmod_libdmview_api(1.0, int, _wait_event, ( uint32_t claim, libdmview_event_t* event, int32_t timeout_ms ));

/**
 * @brief Connect a view to the display's newest claim (called by the
 *        service after it showed the view the claim is for): its properties
 *        and events go through the claim from now on. No claim: none.
 */
dmod_libdmview_api(1.0, int, _attach, ( libdmview_t view, libdmview_display_t display ));

#endif /* LIBDMVIEW_H */
