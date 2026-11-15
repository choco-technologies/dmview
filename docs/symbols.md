# The keyboard of symbols

A small keyboard has no key for some characters, and nobody learns where they are hidden (a password has to be typed, not guessed). The
service shows them on the screen: a view of the service itself, over the lower part of the display, to touch.

## Use

- **MIC key** (the key with the microphone; on its own - with *sym* it is a zero) opens the keyboard, and closes it.
- A touch on a character types it: the application gets it as the key of a keyboard would give it (`"char"` event of its claim, the Unicode
  character). The keyboard **stays open** for the next one; **Close**, **Esc** or the MIC key close it.
- The keys of the keyboard work as before while it is open: the two can be mixed in one word.
- What the keyboard covers is the owner's (or a rent's) again when it closes; what they draw over it while it is open does not stay: the
  keyboard is drawn again over it.

The keyboard shows the 32 punctuation characters of ASCII in the order of their codes and `€ £ ° ± × ÷ ¿ ¡`. It is a view like any
other (`apps/dmview/views/symbols.html`, converted by dmod; 240 x 212 for the e-paper of the T-Deck Pro), so a board with another screen or
another set of characters makes its own - see below.

## Which view

`$DMVIEW_SYMBOLS`, else `/eviews/dmview/symbols.dmv` (the views of the `dmview` module: `dmview @screen <screen>` in `flash.dmd`). No such
file: the MIC key does nothing (once a warning in the log). The service shows it at the bottom of the display, in the middle; its size is the
`.size` of the view and has to fit the display. A turned view (portrait on a landscape display) has no keyboard.

## How the view talks to the service

The view is no claim's, so its `EMIT`s wait in the view: the service takes them with `libdmview_take_event()`:

| Event | Value | Meaning |
|-------|-------|---------|
| `char` | the Unicode character | type it |
| `close` | | close the keyboard |

The touches are the view's while a touch that began in it goes on (the others go to the owner and the rents as before).
