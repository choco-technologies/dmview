#ifndef DMVIEW_FORMAT_H
#define DMVIEW_FORMAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The dmview binary view format (.dmv): the instruction set and the file
 * layout, shared by everything that writes views (libtodmv) and executes
 * them (libdmview). See docs/assembly.md for the instruction set and
 * docs/binary-format.md for the file layout. Everything is little-endian.
 *
 * Header only: the tables below are `static const` plain data (no pointers -
 * the dmod loader does not relocate pointers stored in initialized data) and
 * the helpers are `static inline`, so including this header costs no module
 * dependency.
 */

/** File magic: "DMV" and a terminating zero. */
#define DMV_MAGIC_0              'D'
#define DMV_MAGIC_1              'M'
#define DMV_MAGIC_2              'V'
#define DMV_MAGIC_3              '\0'

/** Format version this header describes. */
#define DMV_VERSION_MAJOR        0
#define DMV_VERSION_MINOR        4

/** Header size: version 0.2 added the gradient tables at its end (0.3:
 * OPACITY, 0.4: ICON - no change to the header). */
#define DMV_HEADER_SIZE          96u
#define DMV_HEADER_SIZE_0_1      80u

/** Instruction header: opcode, size, varmask, flags. */
#define DMV_INSTRUCTION_HEADER_SIZE  4u

/** Most operands one instruction has (one varmask bit each). */
#define DMV_MAX_OPERANDS         8u

/** Longest mnemonic, including the terminator. */
#define DMV_MNEMONIC_MAX         10u

/** "No index" in 16-bit index fields (no env name, no parent, current box). */
#define DMV_NONE                 0xFFFFu

/** Variable indices from here on are built-in variables. */
#define DMV_BUILTIN_BASE         0xFF00u

/** Code offsets (labels, entry) count 4-byte words. */
#define DMV_CODE_WORD            4u

/** Contacts that are not fingers of the input device ($ev.contact). */
#define DMV_POINTER_CONTACT      (-1)
#define DMV_FOCUS_CONTACT        (-2)

/** OPACITY: 0 (invisible) ... DMV_OPACITY_MAX (opaque); values outside are clamped. */
#define DMV_OPACITY_MAX          255

/** Defaults of .longpress and .scrollslop. */
#define DMV_DEFAULT_LONGPRESS_MS 600u
#define DMV_DEFAULT_SCROLLSLOP   8u

/** Opcodes - grouped by category, see dmview's docs/assembly.md. */
typedef enum
{
    /* Structure and flow */
    DMV_OP_NOP       = 0x00,
    DMV_OP_BOX       = 0x01,
    DMV_OP_END       = 0x02,
    DMV_OP_JMP       = 0x03,
    DMV_OP_CALL      = 0x04,
    DMV_OP_RET       = 0x05,
    DMV_OP_JEQ       = 0x06,
    DMV_OP_JNE       = 0x07,
    DMV_OP_JLT       = 0x08,
    DMV_OP_JLE       = 0x09,
    DMV_OP_JGT       = 0x0A,
    DMV_OP_JGE       = 0x0B,
    DMV_OP_SCROLL    = 0x0C,
    DMV_OP_FOCUS     = 0x0D,
    DMV_OP_OPACITY   = 0x0E,

    /* Drawing */
    DMV_OP_FILL      = 0x10,
    DMV_OP_RECT      = 0x11,
    DMV_OP_RRECT     = 0x12,
    DMV_OP_FRAME     = 0x13,
    DMV_OP_RFRAME    = 0x14,
    DMV_OP_LINE      = 0x15,
    DMV_OP_CIRCLE    = 0x16,
    DMV_OP_RING      = 0x17,
    DMV_OP_TEXT      = 0x18,
    DMV_OP_IMAGE     = 0x19,
    DMV_OP_ICON      = 0x1A,

    /* Variables */
    DMV_OP_SET       = 0x40,
    DMV_OP_ADD       = 0x41,
    DMV_OP_SUB       = 0x42,
    DMV_OP_MUL       = 0x43,
    DMV_OP_DIV       = 0x44,
    DMV_OP_MOD       = 0x45,
    DMV_OP_MIN       = 0x46,
    DMV_OP_MAX       = 0x47,
    DMV_OP_CLAMP     = 0x48,
    DMV_OP_TOGGLE    = 0x49,
    DMV_OP_FORMAT    = 0x4A,

    /* Input */
    DMV_OP_ON        = 0x60,

    /* Actions */
    DMV_OP_REDRAW    = 0x80,
    DMV_OP_EXEC      = 0x81,
    DMV_OP_SIGNAL    = 0x82,
    DMV_OP_GOTO      = 0x83,
    DMV_OP_SCROLLTO  = 0x84,
    DMV_OP_RELOAD    = 0x85,
    DMV_OP_SETFOCUS  = 0x86,
} dmv_opcode_t;

/** Instruction categories. */
typedef enum
{
    DMV_CATEGORY_FLOW = 0,
    DMV_CATEGORY_DRAW,
    DMV_CATEGORY_VARIABLE,
    DMV_CATEGORY_INPUT,
    DMV_CATEGORY_ACTION,
} dmv_category_t;

/**
 * Operand kinds. The value kinds (V16, V32, COLOR, STR) hold a variable
 * index instead of an immediate when their varmask bit is set.
 */
typedef enum
{
    DMV_OPERAND_V16 = 1,     /**< 16-bit signed value (coordinate, size) */
    DMV_OPERAND_V32,         /**< 32-bit signed value */
    DMV_OPERAND_COLOR,       /**< 32-bit 0xAARRGGBB value */
    DMV_OPERAND_STR,         /**< 16-bit string index, or string variable */
    DMV_OPERAND_VAR,         /**< 16-bit index of a writable variable */
    DMV_OPERAND_LABEL,       /**< 16-bit code word offset */
    DMV_OPERAND_BOX,         /**< 16-bit box index (DMV_NONE = current box) */
    DMV_OPERAND_FONT,        /**< 16-bit font index */
    DMV_OPERAND_EVENT,       /**< 16-bit dmv_event_t */
} dmv_operand_kind_t;

/** Which flag names the instruction's flags byte takes. */
typedef enum
{
    DMV_FLAGS_NONE = 0,
    DMV_FLAGS_BOX,           /**< DMV_BOX_* */
    DMV_FLAGS_SCROLL,        /**< DMV_SCROLL_* */
    DMV_FLAGS_ALIGN,         /**< DMV_ALIGN_* */
} dmv_flags_kind_t;

/** BOX flags */
#define DMV_BOX_OPAQUE           0x01u
#define DMV_BOX_MULTI            0x02u
#define DMV_BOX_FLAGS_MASK       0x03u

/** SCROLL flags */
#define DMV_SCROLL_HORIZONTAL    0x01u
#define DMV_SCROLL_VERTICAL      0x02u
#define DMV_SCROLL_BAR           0x04u
#define DMV_SCROLL_FLAGS_MASK    0x07u

/** TEXT / IMAGE / ICON alignment flags */
#define DMV_ALIGN_LEFT           0x00u
#define DMV_ALIGN_CENTER         0x01u
#define DMV_ALIGN_RIGHT          0x02u
#define DMV_ALIGN_HMASK          0x03u
#define DMV_ALIGN_TOP            0x00u
#define DMV_ALIGN_MIDDLE         0x04u
#define DMV_ALIGN_BOTTOM         0x08u
#define DMV_ALIGN_VMASK          0x0Cu
#define DMV_ALIGN_WRAP           0x10u
#define DMV_ALIGN_FLAGS_MASK     0x1Fu

/**
 * Paint flag of the drawing instructions with a color operand: the operand
 * holds the index of a gradient (an immediate, never a variable) instead of
 * a color. Next to the instruction's own flags (TEXT's alignment).
 */
#define DMV_PAINT_GRADIENT       0x80u

/** Gradient kinds (.gradient). */
#define DMV_GRADIENT_LINEAR      0u
#define DMV_GRADIENT_RADIAL      1u

/** Color stops of one gradient. */
#define DMV_MIN_STOPS            2u
#define DMV_MAX_STOPS            16u

/** Stop positions are in 1/1000 of the gradient (0 ... 1000). */
#define DMV_STOP_SCALE           1000u

/**
 * Description of one opcode. Plain data only - no pointers: the dmod loader
 * does not relocate pointers stored in initialized data.
 */
typedef struct
{
    char    mnemonic[DMV_MNEMONIC_MAX];      /**< "" for an unused opcode */
    uint8_t category;                           /**< dmv_category_t */
    uint8_t operand_count;                      /**< Number of operands */
    uint8_t optional;                           /**< Trailing operands that may be left out in assembly */
    uint8_t flags_kind;                         /**< dmv_flags_kind_t */
    bool    flags_required;                     /**< The flags must be written in assembly */
    uint8_t operands[DMV_MAX_OPERANDS];      /**< dmv_operand_kind_t of each operand */
} dmv_opcode_info_t;

/** Binary layout of one opcode. */
typedef struct
{
    uint8_t size;                               /**< Total instruction size in bytes */
    uint8_t offsets[DMV_MAX_OPERANDS];       /**< Byte offset of each operand */
} dmv_layout_t;

/** Input events (ON operand). */
typedef enum
{
    DMV_EVENT_PRESS = 0,
    DMV_EVENT_DRAG,
    DMV_EVENT_LONG,
    DMV_EVENT_RELEASE,
    DMV_EVENT_CLICK,
    DMV_EVENT_PINCH,
    DMV_EVENT_ROTATE,
    DMV_EVENT_ENTER,
    DMV_EVENT_LEAVE,
    DMV_EVENT_WHEEL,
    DMV_EVENT_SCROLLED,
    DMV_EVENT_FOCUS,
    DMV_EVENT_BLUR,
    DMV_EVENT_KEY,

    DMV_EVENT_COUNT
} dmv_event_t;

/** Built-in variables - indices from DMV_BUILTIN_BASE. */
typedef enum
{
    DMV_VAR_BOX_W        = 0xFF00,
    DMV_VAR_BOX_H        = 0xFF01,
    DMV_VAR_BOX_PRESSED  = 0xFF02,
    DMV_VAR_BOX_SX       = 0xFF03,
    DMV_VAR_BOX_SY       = 0xFF04,
    DMV_VAR_BOX_CONTACTS = 0xFF05,
    DMV_VAR_BOX_FOCUSED  = 0xFF06,

    DMV_VAR_EV_CONTACT   = 0xFF10,
    DMV_VAR_EV_X         = 0xFF11,
    DMV_VAR_EV_Y         = 0xFF12,
    DMV_VAR_EV_DX        = 0xFF13,
    DMV_VAR_EV_DY        = 0xFF14,
    DMV_VAR_EV_WHEEL     = 0xFF15,
    DMV_VAR_EV_BUTTON    = 0xFF16,
    DMV_VAR_EV_SCALE     = 0xFF17,
    DMV_VAR_EV_ANGLE     = 0xFF18,
    DMV_VAR_EV_CX        = 0xFF19,
    DMV_VAR_EV_CY        = 0xFF1A,
    DMV_VAR_EV_KEY       = 0xFF1B,

    DMV_VAR_VIEW_W       = 0xFF20,
    DMV_VAR_VIEW_H       = 0xFF21,
    DMV_VAR_TIME         = 0xFF22,
} dmv_builtin_var_t;

/** Variable types. */
typedef enum
{
    DMV_VAR_INT = 0,     /**< 32-bit signed integer */
    DMV_VAR_STR,         /**< String of up to `capacity` bytes */
} dmv_var_type_t;

/** Variable flags. */
#define DMV_VARF_ENV             0x01u   /**< Bound to the dmenv variable `env` */

/** View-level items (directives with a handler). */
typedef enum
{
    DMV_ITEM_INIT = 0,   /**< .init label */
    DMV_ITEM_TIMER,      /**< .timer value=ms, label */
    DMV_ITEM_KEY,        /**< .key value=button, label */
    DMV_ITEM_NAVKEY,     /**< .navkeys arg=role, value=button, no label */

    DMV_ITEM_COUNT
} dmv_item_kind_t;

/** Focus navigation roles (.navkeys). */
typedef enum
{
    DMV_NAV_NEXT = 0,
    DMV_NAV_PREV,
    DMV_NAV_UP,
    DMV_NAV_DOWN,
    DMV_NAV_LEFT,
    DMV_NAV_RIGHT,
    DMV_NAV_OK,

    DMV_NAV_COUNT
} dmv_nav_role_t;

/* ---- File structures (dmview's docs/binary-format.md) ---- */

/** Position and number of entries of a table. */
typedef struct
{
    uint32_t offset;    /**< Byte offset from the start of the file, 4-aligned */
    uint32_t count;     /**< Entries (code: 4-byte words) */
} dmv_section_t;

/** File header, at offset 0. */
typedef struct
{
    uint8_t          magic[4];          /**< "DMV\0" */
    uint16_t         version_major;     /**< DMV_VERSION_MAJOR */
    uint16_t         version_minor;     /**< DMV_VERSION_MINOR */
    uint32_t         file_size;         /**< Size of the whole file */
    uint16_t         width;             /**< .size */
    uint16_t         height;
    uint16_t         name;              /**< String index of .view */
    uint16_t         entry;             /**< Code word offset of .entry */
    uint16_t         longpress_ms;      /**< .longpress */
    uint16_t         scrollslop;        /**< .scrollslop */
    dmv_section_t code;              /**< Instructions, count in words */
    dmv_section_t strings;           /**< uint32_t offsets[count] + zero-terminated strings */
    dmv_section_t vars;              /**< dmv_var_t[count] */
    dmv_section_t fonts;             /**< dmv_font_t[count] */
    dmv_section_t boxes;             /**< dmv_box_t[count] */
    dmv_section_t items;             /**< dmv_item_t[count] */
    dmv_section_t symbols;           /**< dmv_symbol_t[count] */
    dmv_section_t gradients;         /**< dmv_gradient_t[count] - version 0.2 */
    dmv_section_t stops;             /**< dmv_stop_t[count] - version 0.2 */
} dmv_header_t;

/** Variable record. */
typedef struct
{
    uint8_t  type;          /**< dmv_var_type_t */
    uint8_t  flags;         /**< DMV_VARF_* */
    uint16_t capacity;      /**< Bytes of a string variable, 0 for an integer */
    uint16_t name;          /**< String index of the name (without '$') */
    uint16_t env;           /**< String index of the dmenv name, or DMV_NONE */
    int32_t  init;          /**< Initial integer, or string index of the initial string */
} dmv_var_t;

/** Font record. */
typedef struct
{
    uint16_t name;          /**< String index of the name */
    uint16_t spec;          /**< String index of the spec, e.g. "sans-16" */
} dmv_font_t;

/**
 * Gradient record. Its geometry is relative to the rectangle of the shape it
 * paints (x, y, w, h of RECT, the box of FILL, ...), so one gradient fits
 * every size.
 */
typedef struct
{
    uint16_t name;          /**< String index of the name */
    uint8_t  kind;          /**< DMV_GRADIENT_* */
    uint8_t  count;         /**< Stops, DMV_MIN_STOPS ... DMV_MAX_STOPS */
    uint16_t first;         /**< Index of its first stop in the stop table */
    int16_t  param[4];      /**< LINEAR: angle in degrees (0 = up, 90 = right), 0, 0, 0;
                                 RADIAL: center x, y in percent of the shape's w, h,
                                 radius x, y in percent of w, h (> 0) */
    uint16_t reserved;      /**< 0 */
} dmv_gradient_t;

/** Color stop of a gradient. */
typedef struct
{
    uint32_t color;         /**< 0xAARRGGBB */
    uint16_t position;      /**< 0 ... DMV_STOP_SCALE, not decreasing within a gradient */
    uint16_t reserved;      /**< 0 */
} dmv_stop_t;

/** Box record. */
typedef struct
{
    uint16_t name;          /**< String index of the name (without '@') */
    uint16_t parent;        /**< Index of the enclosing box, or DMV_NONE */
    uint16_t begin;         /**< Code word offset of its BOX instruction */
    uint16_t end;           /**< Code word offset of its END instruction */
} dmv_box_t;

/** View-level item record. */
typedef struct
{
    uint8_t  kind;          /**< dmv_item_kind_t */
    uint8_t  arg;           /**< DMV_ITEM_NAVKEY: dmv_nav_role_t */
    uint16_t label;         /**< Handler code word offset, or DMV_NONE */
    uint32_t value;         /**< TIMER: ms, KEY / NAVKEY: button index */
} dmv_item_t;

/** Symbol record (labels, for disassembly and debugging). */
typedef struct
{
    uint16_t name;          /**< String index of the label as written (".local" for local labels) */
    uint16_t offset;        /**< Code word offset */
} dmv_symbol_t;

/* ---- Font files (.dmvf, dmview's docs/font-format.md) ---- */

/** Font file magic: "DMVF". */
#define DMVF_MAGIC_0             'D'
#define DMVF_MAGIC_1             'M'
#define DMVF_MAGIC_2             'V'
#define DMVF_MAGIC_3             'F'

#define DMVF_VERSION_MAJOR       0
#define DMVF_VERSION_MINOR       1

/** Bits per pixel of the glyph bitmaps (coverage 0 ... 15). */
#define DMVF_BPP                 4u
#define DMVF_COVERAGE_MAX        15u

/** Font file header, at offset 0. */
typedef struct
{
    uint8_t  magic[4];          /**< "DMVF" */
    uint16_t version_major;     /**< DMVF_VERSION_MAJOR */
    uint16_t version_minor;     /**< DMVF_VERSION_MINOR */
    uint32_t file_size;         /**< Size of the whole file */
    uint16_t size;              /**< Pixel size the font was made for (em) */
    uint16_t line_height;       /**< Distance of two baselines */
    int16_t  ascent;            /**< Baseline below the top of a line */
    int16_t  descent;           /**< Below the baseline */
    uint32_t glyph_count;
    uint32_t glyphs;            /**< Offset of dmvf_glyph_t[glyph_count], by codepoint ascending */
    uint32_t bitmaps;           /**< Offset of the bitmaps */
} dmvf_header_t;

/**
 * Glyph record. Its bitmap: `height` rows of (width + 1) / 2 bytes, two
 * pixels per byte, the left one in the low nibble - the coverage 0 ... 15.
 */
typedef struct
{
    uint32_t bitmap;            /**< Offset from the header's `bitmaps` */
    uint16_t codepoint;         /**< Unicode (BMP) */
    uint8_t  width;             /**< Bitmap size */
    uint8_t  height;
    int8_t   left;              /**< Bitmap's left edge right of the pen */
    int8_t   top;               /**< Bitmap's top edge above the baseline */
    uint8_t  advance;           /**< Pen movement after the glyph */
    uint8_t  reserved;          /**< 0 */
} dmvf_glyph_t;

/* ---- Image files (.dmvi, dmview's docs/image-format.md) ---- */

/** Image file magic: "DMVI". */
#define DMVI_MAGIC_0             'D'
#define DMVI_MAGIC_1             'M'
#define DMVI_MAGIC_2             'V'
#define DMVI_MAGIC_3             'I'

#define DMVI_VERSION_MAJOR       0
#define DMVI_VERSION_MINOR       1

/** Largest palette of an I8 image. */
#define DMVI_PALETTE_MAX         256u

/** Room for the compression name, its terminator included. */
#define DMVI_COMPRESSION_SIZE    12u

/** Pixel formats of an image. */
typedef enum
{
    DMVI_FORMAT_RGB565 = 1,     /**< 16-bit 5:6:5, opaque */
    DMVI_FORMAT_ARGB8888,       /**< 32-bit 0xAARRGGBB */
    DMVI_FORMAT_RGB565A8,       /**< RGB565 pixels and a separate 8-bit alpha plane */
    DMVI_FORMAT_I8,             /**< 8-bit indices into a palette of 0xAARRGGBB colors */
    DMVI_FORMAT_A8,             /**< 8-bit coverage only - a mask, painted by ICON */
    DMVI_FORMAT_A4,             /**< 4-bit coverage, two pixels per byte, the left one in the low nibble */
} dmvi_format_t;

/** Image file header, at offset 0. */
typedef struct
{
    uint8_t  magic[4];          /**< "DMVI" */
    uint16_t version_major;     /**< DMVI_VERSION_MAJOR */
    uint16_t version_minor;     /**< DMVI_VERSION_MINOR */
    uint32_t file_size;         /**< Size of the whole file */
    uint16_t width;             /**< Pixels per row, > 0 */
    uint16_t height;            /**< Rows, > 0 */
    uint8_t  format;            /**< dmvi_format_t */
    uint8_t  reserved;          /**< 0 */
    uint16_t palette_count;     /**< I8: colors in the palette, 1 ... DMVI_PALETTE_MAX; else 0 */
    uint32_t stride;            /**< Bytes from one row of `pixels` to the next */
    uint32_t pixels;            /**< Offset of the pixels, 4-aligned */
    uint32_t alpha_stride;      /**< RGB565A8: bytes from one alpha row to the next; else 0 */
    uint32_t alpha;             /**< RGB565A8: offset of the alpha plane; else 0 */
    uint32_t palette;           /**< I8: offset of uint32_t colors[palette_count], 4-aligned; else 0 */
    char     compression[DMVI_COMPRESSION_SIZE];   /**< Dmod_Compression_* name of what follows the header
                                                        ("fastlz"), zero-padded; empty: not compressed */
    uint32_t unpacked_size;     /**< Bytes after the header once unpacked - the offsets above count in
                                     the unpacked image */
} dmvi_header_t;

/** Result of dmv_validate(). */
typedef enum
{
    DMV_VALID = 0,
    DMV_ERR_ARGUMENT,    /**< NULL data */
    DMV_ERR_HEADER,      /**< Too small, wrong magic or size */
    DMV_ERR_VERSION,     /**< Unsupported format version */
    DMV_ERR_SECTION,     /**< A table lies outside the file or is misaligned */
    DMV_ERR_STRING,      /**< A string is not terminated inside its table */
    DMV_ERR_OPCODE,      /**< Unknown opcode */
    DMV_ERR_SIZE,        /**< Instruction size does not match its opcode */
    DMV_ERR_OPERAND,     /**< Operand out of range (index, varmask, flags) */
    DMV_ERR_LABEL,       /**< Code offset not at an instruction */
    DMV_ERR_NESTING,     /**< BOX / END unbalanced or not matching the box table */
    DMV_ERR_TABLE,       /**< Invalid table entry */
    DMV_ERR_MEMORY,      /**< Out of memory while validating */
    DMV_ERR_IO,          /**< Reading the view failed */
} dmv_status_t;

/* ---- Reading a view in pieces ---- */

/** Read @p size bytes at @p offset of a view; 0 on success. */
typedef int (*dmv_read_fn)(void *ctx, uint32_t offset, void *buffer, size_t size);

/** A binary view, read in pieces at random offsets. */
typedef struct
{
    dmv_read_fn     read;
    void           *ctx;
    uint32_t        size;       /**< Size of the whole view */
} dmv_input_t;

/* ---- Instruction set ----
 *
 * Indexed by opcode. Operand layouts follow from the operand kinds: operands
 * come after the 4-byte header in order, each aligned to its own size (2 or
 * 4 bytes), the instruction is padded to a multiple of 4 (dmv_get_layout()). */

#define V16     DMV_OPERAND_V16
#define V32     DMV_OPERAND_V32
#define COLOR   DMV_OPERAND_COLOR
#define STR     DMV_OPERAND_STR
#define VAR     DMV_OPERAND_VAR
#define LABEL   DMV_OPERAND_LABEL
#define BOX     DMV_OPERAND_BOX
#define FONT    DMV_OPERAND_FONT
#define EVENT   DMV_OPERAND_EVENT

#define OP(op, name, category, count, optional, flags_kind, flags_required, ...) \
    [op] = { name, category, count, optional, flags_kind, flags_required, { __VA_ARGS__ } }

#define FLOW    DMV_CATEGORY_FLOW
#define DRAW    DMV_CATEGORY_DRAW
#define VARIA   DMV_CATEGORY_VARIABLE
#define INPUT   DMV_CATEGORY_INPUT
#define ACTION  DMV_CATEGORY_ACTION

#define F_NONE  DMV_FLAGS_NONE
#define F_BOX   DMV_FLAGS_BOX
#define F_SCR   DMV_FLAGS_SCROLL
#define F_ALIGN DMV_FLAGS_ALIGN

#define DMV_OPCODE_TABLE_SIZE   (DMV_OP_SETFOCUS + 1)

static const dmv_opcode_info_t dmv_opcode_table[DMV_OPCODE_TABLE_SIZE] = {
    OP(DMV_OP_NOP,      "NOP",      FLOW,   0, 0, F_NONE,  false, 0),
    OP(DMV_OP_BOX,      "BOX",      FLOW,   5, 0, F_BOX,   false, BOX, V16, V16, V16, V16),
    OP(DMV_OP_END,      "END",      FLOW,   0, 0, F_NONE,  false, 0),
    OP(DMV_OP_JMP,      "JMP",      FLOW,   1, 0, F_NONE,  false, LABEL),
    OP(DMV_OP_CALL,     "CALL",     FLOW,   1, 0, F_NONE,  false, LABEL),
    OP(DMV_OP_RET,      "RET",      FLOW,   0, 0, F_NONE,  false, 0),
    OP(DMV_OP_JEQ,      "JEQ",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_JNE,      "JNE",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_JLT,      "JLT",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_JLE,      "JLE",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_JGT,      "JGT",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_JGE,      "JGE",      FLOW,   3, 0, F_NONE,  false, V32, V32, LABEL),
    OP(DMV_OP_SCROLL,   "SCROLL",   FLOW,   2, 0, F_SCR,   false, V16, V16),
    OP(DMV_OP_FOCUS,    "FOCUS",    FLOW,   1, 0, F_NONE,  false, V16),
    OP(DMV_OP_OPACITY,  "OPACITY",  FLOW,   1, 0, F_NONE,  false, V16),

    OP(DMV_OP_FILL,     "FILL",     DRAW,   1, 0, F_NONE,  false, COLOR),
    OP(DMV_OP_RECT,     "RECT",     DRAW,   5, 0, F_NONE,  false, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_RRECT,    "RRECT",    DRAW,   6, 0, F_NONE,  false, V16, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_FRAME,    "FRAME",    DRAW,   6, 0, F_NONE,  false, V16, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_RFRAME,   "RFRAME",   DRAW,   7, 0, F_NONE,  false, V16, V16, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_LINE,     "LINE",     DRAW,   6, 0, F_NONE,  false, V16, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_CIRCLE,   "CIRCLE",   DRAW,   4, 0, F_NONE,  false, V16, V16, V16, COLOR),
    OP(DMV_OP_RING,     "RING",     DRAW,   5, 0, F_NONE,  false, V16, V16, V16, V16, COLOR),
    OP(DMV_OP_TEXT,     "TEXT",     DRAW,   7, 0, F_ALIGN, true,  V16, V16, V16, V16, STR, FONT, COLOR),
    OP(DMV_OP_IMAGE,    "IMAGE",    DRAW,   5, 0, F_ALIGN, true,  V16, V16, V16, V16, STR),
    OP(DMV_OP_ICON,     "ICON",     DRAW,   6, 0, F_ALIGN, true,  V16, V16, V16, V16, STR, COLOR),

    OP(DMV_OP_SET,      "SET",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_ADD,      "ADD",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_SUB,      "SUB",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_MUL,      "MUL",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_DIV,      "DIV",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_MOD,      "MOD",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_MIN,      "MIN",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_MAX,      "MAX",      VARIA,  2, 0, F_NONE,  false, VAR, V32),
    OP(DMV_OP_CLAMP,    "CLAMP",    VARIA,  3, 0, F_NONE,  false, VAR, V32, V32),
    OP(DMV_OP_TOGGLE,   "TOGGLE",   VARIA,  1, 0, F_NONE,  false, VAR),
    OP(DMV_OP_FORMAT,   "FORMAT",   VARIA,  3, 0, F_NONE,  false, VAR, STR, V32),

    OP(DMV_OP_ON,       "ON",       INPUT,  2, 0, F_NONE,  false, EVENT, LABEL),

    OP(DMV_OP_REDRAW,   "REDRAW",   ACTION, 1, 1, F_NONE,  false, BOX),
    OP(DMV_OP_EXEC,     "EXEC",     ACTION, 1, 0, F_NONE,  false, STR),
    OP(DMV_OP_SIGNAL,   "SIGNAL",   ACTION, 1, 0, F_NONE,  false, STR),
    OP(DMV_OP_GOTO,     "GOTO",     ACTION, 1, 0, F_NONE,  false, STR),
    OP(DMV_OP_SCROLLTO, "SCROLLTO", ACTION, 3, 0, F_NONE,  false, BOX, V16, V16),
    OP(DMV_OP_RELOAD,   "RELOAD",   ACTION, 1, 0, F_NONE,  false, STR),
    OP(DMV_OP_SETFOCUS, "SETFOCUS", ACTION, 1, 0, F_NONE,  false, BOX),
};

#undef V16
#undef V32
#undef COLOR
#undef STR
#undef VAR
#undef LABEL
#undef BOX
#undef FONT
#undef EVENT
#undef OP
#undef FLOW
#undef DRAW
#undef VARIA
#undef INPUT
#undef ACTION
#undef F_NONE
#undef F_BOX
#undef F_SCR
#undef F_ALIGN

static inline const dmv_opcode_info_t* dmv_get_opcode_info(uint8_t opcode)
{
    if (opcode >= DMV_OPCODE_TABLE_SIZE || dmv_opcode_table[opcode].mnemonic[0] == '\0')
        return NULL;
    return &dmv_opcode_table[opcode];
}

static inline uint8_t dmv_operand_size(uint8_t kind)
{
    switch (kind)
    {
        case DMV_OPERAND_V32:
        case DMV_OPERAND_COLOR:
            return 4;
        case DMV_OPERAND_V16:
        case DMV_OPERAND_STR:
        case DMV_OPERAND_VAR:
        case DMV_OPERAND_LABEL:
        case DMV_OPERAND_BOX:
        case DMV_OPERAND_FONT:
        case DMV_OPERAND_EVENT:
            return 2;
        default:
            return 0;
    }
}

static inline bool dmv_get_layout(uint8_t opcode, dmv_layout_t* layout)
{
    const dmv_opcode_info_t *info = dmv_get_opcode_info(opcode);
    if (info == NULL || layout == NULL)
        return false;

    uint32_t offset = DMV_INSTRUCTION_HEADER_SIZE;
    for (uint8_t i = 0; i < DMV_MAX_OPERANDS; i++)
        layout->offsets[i] = 0;
    for (uint8_t i = 0; i < info->operand_count; i++)
    {
        uint8_t size = dmv_operand_size(info->operands[i]);
        offset = (offset + size - 1U) & ~(uint32_t)(size - 1U);
        layout->offsets[i] = (uint8_t)offset;
        offset += size;
    }
    layout->size = (uint8_t)((offset + 3U) & ~3U);
    return true;
}

static inline const char* dmv_status_name(dmv_status_t status)
{
    switch (status)
    {
        case DMV_VALID:          return "valid";
        case DMV_ERR_ARGUMENT:   return "invalid argument";
        case DMV_ERR_HEADER:     return "invalid header";
        case DMV_ERR_VERSION:    return "unsupported format version";
        case DMV_ERR_SECTION:    return "table outside the file";
        case DMV_ERR_STRING:     return "unterminated string";
        case DMV_ERR_OPCODE:     return "unknown opcode";
        case DMV_ERR_SIZE:       return "wrong instruction size";
        case DMV_ERR_OPERAND:    return "operand out of range";
        case DMV_ERR_LABEL:      return "code offset not at an instruction";
        case DMV_ERR_NESTING:    return "BOX/END mismatch";
        case DMV_ERR_TABLE:      return "invalid table entry";
        case DMV_ERR_MEMORY:     return "out of memory";
        case DMV_ERR_IO:         return "read error";
        default:                    return "unknown status";
    }
}

#endif /* DMVIEW_FORMAT_H */
