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
 * @brief Draw @p area on the next libdmview_render(): what something else drew over it (a rented rectangle - see
 *        libdmview_rent_create()) is the view's again.
 */
dmod_libdmview_api(1.0, void, _invalidate_rect, ( libdmview_t view, const libdmview_rect_t* area ));

/**
 * @brief Feed the current state of the input device.
 *
 * Turns changes into events (PRESS, DRAG, RELEASE, CLICK, .key) and runs
 * their handlers. Coordinates are surface coordinates.
 */
dmod_libdmview_api(1.0, int, _input, ( libdmview_t view, const dmdrvi_input_state_t* state, uint32_t now_ms ));

/**
 * @brief A key of a keyboard, for the application that claimed the display. A view has no text
 *        fields: what is typed is the application's, so the key goes to it as events of the claim
 *        (libdmview_wait_event()):
 *          - "mods": the modifiers changed - value: the bits (LIBINPUT_MOD_*: shift 1, ctrl 2, alt 4,
 *            sym 8, caps 16),
 *          - "char": a key that gives a character went down - value: the Unicode character,
 *          - "key":  a key that gives none went down (Enter, Backspace, an arrow) - value: its
 *            USB HID usage.
 *        A modifier key gives "mods" only, a release gives nothing but a change of "mods".
 *
 * @param code      The key: a USB HID usage
 * @param codepoint What it gave in the layer in effect, 0: none
 * @param down      Went down (else: up)
 * @param modifiers The modifiers in effect
 * @return 0, -EINVAL
 */
dmod_libdmview_api(1.0, int, _key, ( libdmview_t view, uint16_t code, uint32_t codepoint, bool down, uint8_t modifiers ));

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

/* ---- Screen rent ----
 *
 * The application that owns a display - phoneui, carhmi: the one that has its claim on top - can rent a rectangle of its view to
 * another application, which then gets only that part of the screen. The owner decides where it is, how big, whether it is shown
 * and whether the keys go to it; the application knows nothing of the rest of the screen, and the service draws it - the owner's
 * view first, the rented views over it, into the same framebuffer: nobody but the service ever touches the pixels.
 *
 * The owner creates a rent (libdmview_rent_create()), starts the application and gives it the name of the rent
 * (libdmview_rent_name()), usually as an argument (`--display NAME`); the application claims that name as it would claim a display:
 * libdmview_claim(name, view, &claim). It is a claim like any other - the channel of properties and events is the same - but it
 * does not take the display: its view is shown in the rectangle. What the application is told of its part:
 *  - the view draws into a surface of the size of the rectangle, so `$view_w` and `$view_h` are that size - a view that lays
 *    itself out by them adapts, and when the owner resizes the rent it is laid out again;
 *  - libdmview_rent_get_size() tells the application the same;
 *  - the events "rent.size" (value: width << 16 | height), "rent.visible" and "rent.focus" (value 0 or 1) are queued in its claim.
 * The owner learns of its rents from the events of its own claim: "rent.open" when an application took one and "rent.closed"
 * when it went (released its claim, or ended) - the value is the id of the rent. A rent the owner took back is gone, with no event.
 *
 * Touches go to the rent whose rectangle they began in, with coordinates relative to it, until they are lifted; the rest are the
 * owner's. Keys go to the rent that has the focus, except the escape key of that rent (the owner's, e.g. Esc: it can always take
 * the control back). Rents do not overlap, and a rent cannot be rented further.
 */

/**
 * @brief Rent @p rect of the view of @p claim (the coordinates of that view) - to be taken by an application.
 *
 * @param claim A claim on a display, of the calling process
 * @param rent  Receives the rent
 * @return 0, -ENOENT (no such claim), -EPERM (not the process of the claim), -EINVAL (an empty rectangle, or the claim is
 *         an application's in a rent), -EEXIST (it overlaps another rent of the claim), -ENOMEM
 */
dmod_libdmview_api(1.0, int, _rent_create, ( uint32_t claim, const libdmview_rect_t* rect, uint32_t* rent ));

/**
 * @brief The name an application claims the rent by: "<display>/<rent>" (libdmview_claim() takes it in place of a display).
 * @return 0, -ENOENT, -ERANGE (cut)
 */
dmod_libdmview_api(1.0, int, _rent_name, ( uint32_t rent, char* name, size_t size ));

/**
 * @brief Only the process @p pid may take the rent (libdmview_claim() fails with -EACCES for any other); 0: any process,
 *        as long as it has not been taken. A process that has gone leaves the rent empty: it is bound again for the next one.
 * @return 0, -ENOENT, -EPERM
 */
dmod_libdmview_api(1.0, int, _rent_bind, ( uint32_t rent, uint32_t pid ));

/** @brief Move or resize the rent; the application learns the size ("rent.size"). -ENOENT, -EPERM, -EINVAL, -EEXIST. */
dmod_libdmview_api(1.0, int, _rent_set_rect, ( uint32_t rent, const libdmview_rect_t* rect ));

/** @brief Show the rent or not (an application that is not shown is not drawn, takes no input and runs no timers): "rent.visible". -ENOENT, -EPERM. */
dmod_libdmview_api(1.0, int, _rent_set_visible, ( uint32_t rent, bool visible ));

/** @brief The keys go to this rent (and to no other of the claim): "rent.focus" - false: to the owner. -ENOENT, -EPERM. */
dmod_libdmview_api(1.0, int, _rent_set_focus, ( uint32_t rent, bool focus ));

/** @brief The key (USB HID usage) that is always the owner's, though the rent has the focus; 0: none. -ENOENT, -EPERM. */
dmod_libdmview_api(1.0, int, _rent_set_escape, ( uint32_t rent, uint16_t key ));

/** @brief Take the rent back: the application's claim goes (-ENOENT for it) and the rent with it. -ENOENT, -EPERM. */
dmod_libdmview_api(1.0, int, _rent_release, ( uint32_t rent ));

/**
 * @brief The size of the rent a claim is in - for an application that was given part of a screen.
 * @return 0, -ENOENT (no such claim, or it is not in a rent)
 */
dmod_libdmview_api(1.0, int, _rent_get_size, ( uint32_t claim, uint16_t* width, uint16_t* height ));

/**
 * @brief The rents of the claim on top of the display (the one that is shown), for the service.
 *
 * Applications that have gone are dropped from them here. @p generation changes whenever the answer may have changed.
 *
 * @param out   Room for @p max rents
 * @param count Receives how many were stored
 * @return 0, -EINVAL
 */
dmod_libdmview_api(1.0, int, _display_rents, ( libdmview_display_t display, uint32_t* generation, libdmview_rent_info_t* out, uint32_t max, uint32_t* count ));

/** @brief Generation of the rents of a display - see libdmview_display_rents(). */
dmod_libdmview_api(1.0, uint32_t, _display_rent_generation, ( libdmview_display_t display ));

/** @brief libdmview_attach() for the view of an application in a rent: its properties and events go through @p claim. -ENOENT (not a claim in a rent), -EINVAL. */
dmod_libdmview_api(1.0, int, _rent_attach, ( libdmview_t view, uint32_t claim ));

/* ---- Composing the rents (for the service of a display) ----
 *
 * The service shows the view of the claim on top of the display as before and gives the rest to a libdmview_rents_t: it opens the
 * views of the applications in its rents, draws them over the owner's view, routes the touches and the keys. Per frame:
 *
 *     libdmview_rents_sync(rents, &vacated);               // rents that came, changed or went
 *     if (vacated.w) libdmview_invalidate_rect(owner_view, &vacated);
 *     ... touches and keys: libdmview_rents_input() / libdmview_rents_key() first, what is left is the owner's ...
 *     deadline = min(libdmview_update(owner_view, now), libdmview_rents_update(rents, now));
 *     libdmview_render(owner_view, surface, &changed);     // then
 *     libdmview_rents_damage(rents, &changed);             // what the owner drew over is theirs again
 *     libdmview_rents_render(rents, surface, &rented);     // and the union of the two is what changed
 *
 * The coordinates are those of the owner's view (the service turns touches back before it routes them, as it does for the view).
 */

/** @brief The rents of a display's shown claim. NULL when out of memory. */
dmod_libdmview_api(1.0, libdmview_rents_t, _rents_create, ( libdmview_display_t display ));

/** @brief Close the views of the rents. Safe on NULL. */
dmod_libdmview_api(1.0, void, _rents_destroy, ( libdmview_rents_t rents ));

/**
 * @brief Bring the views of the rents up to date with the display's: open those of applications that came, close those that
 *        went or changed their view, take in the rectangles.
 * @param vacated Receives the rectangle that was a rent's and is not (it went, moved, got smaller or is not shown): the owner's
 *                view is to be drawn there again (libdmview_invalidate_rect()); empty when none (may be NULL)
 * @return true when something changed
 */
dmod_libdmview_api(1.0, bool, _rents_sync, ( libdmview_rents_t rents, libdmview_rect_t* vacated ));

/** @brief Run what is due in the shown rents (.timer, LONG); returns the ms until the next deadline, LIBDMVIEW_NO_DEADLINE when none. */
dmod_libdmview_api(1.0, uint32_t, _rents_update, ( libdmview_rents_t rents, uint32_t now_ms ));

/** @brief The owner drew @p area over the rents: what of it is theirs they draw again on the next libdmview_rents_render(). */
dmod_libdmview_api(1.0, void, _rents_damage, ( libdmview_rents_t rents, const libdmview_rect_t* area ));

/**
 * @brief Draw what changed in the shown rents into @p surface - each view into its rectangle, as a surface of its size.
 * @param changed Receives the bounding rectangle of what was drawn, in the coordinates of @p surface (may be NULL)
 * @return 1 when something was drawn, 0 when not, a negative errno (libdmview_render()) when a view could not be drawn
 */
dmod_libdmview_api(1.0, int, _rents_render, ( libdmview_rents_t rents, const libdmview_surface_t* surface, libdmview_rect_t* changed ));

/**
 * @brief Route the contacts of @p state: those that began in a rent go to its view (with coordinates relative to the rent,
 *        until they are lifted), and are taken out of @p state, which is left for the owner's view.
 * @return true when a rent got a contact
 */
dmod_libdmview_api(1.0, bool, _rents_input, ( libdmview_rents_t rents, dmdrvi_input_state_t* state, uint32_t now_ms ));

/**
 * @brief A key for the rent with the focus (see libdmview_key()).
 * @return true when the rent took it - false: it is the owner's (no rent has the focus, or it is the escape key)
 */
dmod_libdmview_api(1.0, bool, _rents_key, ( libdmview_rents_t rents, uint16_t code, uint32_t codepoint, bool down, uint8_t modifiers ));

#endif /* LIBDMVIEW_H */
