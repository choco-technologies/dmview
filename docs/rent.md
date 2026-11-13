# Screen rent

An application that owns a display - phoneui, carhmi: the one whose claim is on top - decides what the whole screen shows. Some of what
runs on a phone is not meant to have the whole screen, only the part the owner gives it: a map in a corner, a terminal under the status bar,
an application that is launched from the launcher. The owner **rents** it a rectangle of its view.

Nobody but the service touches the pixels. A rented application, like any other, gives libdmview a view and talks to it through the
channel of a claim (properties and events); the service draws that view into the rectangle, over the owner's, into the same framebuffer.
That keeps the memory of an application small, the display one writer, and the e-paper's partial refresh working: what the rented view
changes is a rectangle of the screen.

## How it works

```
owner (phoneui)                              application                          service (dmview)
  claim = libdmview_claim("epd0", ui.dmv)
  rent  = libdmview_rent_create(claim, {0,24,240,276})
  libdmview_rent_name(rent, name)            -> "epd0/3"
  spawn the application with --display epd0/3
  libdmview_rent_bind(rent, pid)
                                             libdmview_claim("epd0/3", app.dmv, &c)
                                                                                    sync: opens app.dmv, attaches its claim
  "rent.open" in the owner's claim <---------
                                                                                    draws app.dmv into {0,24,240,276}
                                                                                    touches that began there -> app.dmv
```

1. **The owner rents.** `libdmview_rent_create()` with a rectangle in the coordinates of its own view. Rents of a claim do not overlap and
   belong to the claim: they go with it. `libdmview_rent_name()` is the name the application claims, `"<display>/<rent>"`. The owner
   starts the application and passes the name (usually `--display NAME`), and may `libdmview_rent_bind()` the rent to the process, so that
   no other can take it.
2. **The application claims the name** with `libdmview_claim()`, as it would a display. It is a claim like any other: the properties and
   events of its view are the same. It does not take the display - the display still shows the owner's view - and the application is not
   told anything about the rest of the screen.
3. **The service draws it.** For each rent of the claim on top the service holds a view, and draws it, as a surface of the rectangle's size, into
   that part of the framebuffer; the owner's view first. A view lays itself out by `$view.w` and `$view.h` - the size it is given -, so
   when the owner resizes the rent (`libdmview_rent_set_rect()`) it is laid out again. The application is told the size as well:
   `libdmview_rent_get_size()` and the event `"rent.size"`.
4. **Input.** A touch goes to the rent its first contact was in - with coordinates relative to the rent - until it is lifted, wherever it
   moves; the others are the owner's. Keys go to the rent that has the focus (`libdmview_rent_set_focus()`), except its escape key
   (`libdmview_rent_set_escape()`, e.g. Esc): that one is the owner's, which can always take the control back.
5. **Coming and going.** The owner is told in its own claim: `"rent.open"` when an application took a rent and `"rent.closed"` when it left
   (released the claim, or ended). The rent stays and can be taken again. When the owner takes the rent back
   (`libdmview_rent_release()`) it is gone, with no event, and the application's claim gets `-ENOENT`, like an application whose display is gone. `libdmview_rent_set_visible(false)`
   puts an application aside: it is not drawn, takes no input and runs no timers, and is told (`"rent.visible"`).

Turning a view (a portrait view on a landscape display) works for the whole frame as before: the rectangles are in the owner's view, and
the service turns the touches back before it routes them.

## For the service

`libdmview_rents_t` does the work (the functions are documented in [api-reference.md](api-reference.md#screen-rent)); per frame:

```c
libdmview_rents_sync(rents, &vacated);                 // rents that came, changed or went
if (vacated.w) libdmview_invalidate_rect(owner, &vacated);
...                                                    // touches: libdmview_rents_input(), the rest is the owner's; keys: libdmview_rents_key()
deadline = min(libdmview_update(owner, now), libdmview_rents_update(rents, now));
libdmview_render(owner, surface, &changed);
libdmview_rents_damage(rents, &changed);               // what the owner drew over a rent is the rent's again
libdmview_rents_render(rents, surface, &rented);       // the union of the two is what changed
```

A rectangle that is partly off the surface is clipped, and the application sees what is left. A rent whose application's view cannot be
opened stays empty (an error in the log).

## What it is not

- **Not a window system**: no overlapping rents, no stacking, no framebuffer for an application, no scaling. The owner lays out its screen.
- **Not nested**: a claim in a rent cannot rent further.
- **Touch only for now**: a mouse (buttons, motion, wheel) is the owner's - its position is not known to the service.
