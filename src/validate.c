#include "private.h"
#include <string.h>

/* ---- Validation ----
 *
 * The view is read in small pieces through the input; besides a few
 * hundred bytes of stack, only one bit per code word (instruction starts)
 * and one byte per variable (its type) are allocated. */

#define MAX_BOX_DEPTH       32u
#define MAX_INSTRUCTION     64u
#define STRING_CHUNK        32u

typedef struct
{
    const dmv_input_t *in;
    dmv_header_t            h;
    uint32_t                header_size;    /* DMV_HEADER_SIZE, or _0_1 for a version 0.1 view */
    uint8_t                *boundaries;     /* One bit per code word: an instruction starts there */
    uint8_t                *var_types;      /* Type of every variable */
    uint32_t                error_offset;
    bool                    io_failed;
} validator_t;

static dmv_status_t fail(validator_t *v, dmv_status_t status, uint32_t offset)
{
    v->error_offset = offset;
    return v->io_failed ? DMV_ERR_IO : status;
}

/* Read `size` bytes at `offset`; false if outside the view or on an error. */
static bool rd(validator_t *v, uint32_t offset, void *buffer, uint32_t size)
{
    if ((uint64_t)offset + size > v->in->size)
        return false;
    if (v->in->read(v->in->ctx, offset, buffer, size) != 0)
    {
        v->io_failed = true;
        return false;
    }
    return true;
}

static void read_section(const uint8_t *p, dmv_section_t *s)
{
    s->offset = rd32(p);
    s->count  = rd32(p + 4);
}

static void read_header(const uint8_t *p, dmv_header_t *h)
{
    memcpy(h->magic, p, 4);
    h->version_major = rd16(p + 4);
    h->version_minor = rd16(p + 6);
    h->file_size     = rd32(p + 8);
    h->width         = rd16(p + 12);
    h->height        = rd16(p + 14);
    h->name          = rd16(p + 16);
    h->entry         = rd16(p + 18);
    h->longpress_ms  = rd16(p + 20);
    h->scrollslop    = rd16(p + 22);
    read_section(p + 24, &h->code);
    read_section(p + 32, &h->strings);
    read_section(p + 40, &h->vars);
    read_section(p + 48, &h->fonts);
    read_section(p + 56, &h->boxes);
    read_section(p + 64, &h->items);
    read_section(p + 72, &h->symbols);
    if (h->version_minor >= 2)
    {
        read_section(p + 80, &h->gradients);
        read_section(p + 88, &h->stops);
    }
}

static bool section_fits(const validator_t *v, const dmv_section_t *s, uint32_t entry_size)
{
    uint64_t end = (uint64_t)s->offset + (uint64_t)s->count * entry_size;
    if (s->count == 0)
        return s->offset <= v->in->size;
    return (s->offset % 4U) == 0 && s->offset >= v->header_size && end <= v->in->size;
}

static bool string_valid(const validator_t *v, uint32_t index)
{
    return index < v->h.strings.count;
}

static bool is_boundary(const validator_t *v, uint32_t word)
{
    return word < v->h.code.count && (v->boundaries[word / 8U] & (1U << (word % 8U))) != 0;
}

static bool builtin_known(uint32_t index)
{
    return (index >= DMV_VAR_BOX_W && index <= DMV_VAR_BOX_FOCUSED) ||
           (index >= DMV_VAR_EV_CONTACT && index <= DMV_VAR_EV_KEY) ||
           (index >= DMV_VAR_VIEW_W && index <= DMV_VAR_TIME);
}

/* Type of variable `index`: DMV_VAR_INT / _STR, or -1 if there is none.
 * Built-in variables are integers. */
static int var_type(const validator_t *v, uint32_t index)
{
    if (index >= DMV_BUILTIN_BASE)
        return builtin_known(index) ? DMV_VAR_INT : -1;
    return (index < v->h.vars.count) ? v->var_types[index] : -1;
}

static dmv_status_t check_strings(validator_t *v)
{
    const dmv_section_t *s = &v->h.strings;
    uint64_t text_start = (uint64_t)s->offset + (uint64_t)s->count * 4U;

    for (uint32_t i = 0; i < s->count; i++)
    {
        uint8_t entry[4], chunk[STRING_CHUNK];
        uint32_t entry_at = s->offset + i * 4U;
        if (!rd(v, entry_at, entry, 4))
            return fail(v, DMV_ERR_STRING, entry_at);

        uint64_t at = (uint64_t)s->offset + rd32(entry);
        if (at < text_start || at >= v->in->size)
            return fail(v, DMV_ERR_STRING, entry_at);

        /* Find the terminator inside the view */
        for (bool terminated = false; !terminated; )
        {
            uint32_t n = (uint32_t)((v->in->size - at < STRING_CHUNK) ? v->in->size - at : STRING_CHUNK);
            if (n == 0 || !rd(v, (uint32_t)at, chunk, n))
                return fail(v, DMV_ERR_STRING, entry_at);
            for (uint32_t k = 0; k < n && !terminated; k++)
                terminated = chunk[k] == '\0';
            at += n;
        }
    }
    return DMV_VALID;
}

static dmv_status_t check_vars(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.vars.count; i++)
    {
        uint8_t p[sizeof(dmv_var_t)];
        uint32_t at = v->h.vars.offset + i * (uint32_t)sizeof(dmv_var_t);
        if (!rd(v, at, p, sizeof(p)))
            return fail(v, DMV_ERR_TABLE, at);

        uint8_t  type = p[0], flags = p[1];
        uint16_t capacity = rd16(p + 2), name = rd16(p + 4), env = rd16(p + 6);
        uint32_t init = rd32(p + 8);

        bool ok = (type == DMV_VAR_INT && capacity == 0) ||
                  (type == DMV_VAR_STR && capacity > 0 && string_valid(v, init));
        ok = ok && (flags & ~DMV_VARF_ENV) == 0 && string_valid(v, name);
        ok = ok && ((flags & DMV_VARF_ENV) ? string_valid(v, env) : env == DMV_NONE);
        if (!ok)
            return fail(v, DMV_ERR_TABLE, at);
        v->var_types[i] = type;
    }
    return DMV_VALID;
}

static dmv_status_t check_fonts(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.fonts.count; i++)
    {
        uint8_t p[sizeof(dmv_font_t)];
        uint32_t at = v->h.fonts.offset + i * (uint32_t)sizeof(dmv_font_t);
        if (!rd(v, at, p, sizeof(p)) || !string_valid(v, rd16(p)) || !string_valid(v, rd16(p + 2)))
            return fail(v, DMV_ERR_TABLE, at);
    }
    return DMV_VALID;
}

static dmv_status_t check_gradients(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.gradients.count; i++)
    {
        uint8_t p[sizeof(dmv_gradient_t)];
        uint32_t at = v->h.gradients.offset + i * (uint32_t)sizeof(dmv_gradient_t);
        if (!rd(v, at, p, sizeof(p)))
            return fail(v, DMV_ERR_TABLE, at);

        uint8_t kind = p[2], count = p[3];
        uint16_t first = rd16(p + 4);
        int16_t p0 = (int16_t)rd16(p + 6), p1 = (int16_t)rd16(p + 8), p2 = (int16_t)rd16(p + 10),
                p3 = (int16_t)rd16(p + 12);
        bool ok = string_valid(v, rd16(p)) && rd16(p + 14) == 0 &&
                  count >= DMV_MIN_STOPS && count <= DMV_MAX_STOPS &&
                  (uint32_t)first + count <= v->h.stops.count;
        if (kind == DMV_GRADIENT_LINEAR)
            ok = ok && p0 >= 0 && p0 < 360 && p1 == 0 && p2 == 0 && p3 == 0;
        else
            ok = ok && kind == DMV_GRADIENT_RADIAL && p2 > 0 && p3 > 0;

        /* Its stops: in range and in order */
        uint32_t previous = 0;
        for (uint32_t k = 0; ok && k < count; k++)
        {
            uint8_t stop[sizeof(dmv_stop_t)];
            uint32_t stop_at = v->h.stops.offset + (first + k) * (uint32_t)sizeof(dmv_stop_t);
            if (!rd(v, stop_at, stop, sizeof(stop)))
                return fail(v, DMV_ERR_TABLE, stop_at);
            uint16_t position = rd16(stop + 4);
            ok = position <= DMV_STOP_SCALE && position >= previous && rd16(stop + 6) == 0;
            previous = position;
        }
        if (!ok)
            return fail(v, DMV_ERR_TABLE, at);
    }
    return DMV_VALID;
}

/* Index of the color operand of a drawing instruction, -1 if it has none. */
static int color_operand(const dmv_opcode_info_t *info)
{
    if (info->category != DMV_CATEGORY_DRAW)
        return -1;
    for (uint8_t i = 0; i < info->operand_count; i++)
    {
        if (info->operands[i] == DMV_OPERAND_COLOR)
            return i;
    }
    return -1;
}

/* First pass: every instruction is known and has its opcode's size. */
static dmv_status_t mark_instructions(validator_t *v)
{
    uint32_t code_size = v->h.code.count * DMV_CODE_WORD;
    uint32_t pos = 0;

    while (pos < code_size)
    {
        uint8_t head[2];
        uint32_t at = v->h.code.offset + pos;
        dmv_layout_t layout;
        if (!rd(v, at, head, sizeof(head)))
            return fail(v, DMV_ERR_SIZE, at);
        if (!dmv_get_layout(head[0], &layout))
            return fail(v, DMV_ERR_OPCODE, at);
        if (head[1] != layout.size || pos + layout.size > code_size)
            return fail(v, DMV_ERR_SIZE, at);
        v->boundaries[(pos / 4U) / 8U] |= (uint8_t)(1U << ((pos / 4U) % 8U));
        pos += layout.size;
    }
    return DMV_VALID;
}

static bool flags_valid(uint8_t flags_kind, uint8_t flags)
{
    switch (flags_kind)
    {
        case DMV_FLAGS_BOX:    return (flags & ~DMV_BOX_FLAGS_MASK) == 0;
        case DMV_FLAGS_SCROLL: return (flags & ~DMV_SCROLL_FLAGS_MASK) == 0;
        case DMV_FLAGS_ALIGN:  return (flags & ~DMV_ALIGN_FLAGS_MASK) == 0 &&
                                      (flags & DMV_ALIGN_HMASK) != 0x03u &&
                                      (flags & DMV_ALIGN_VMASK) != 0x0Cu;
        default:               return flags == 0;
    }
}

static bool is_value_kind(uint8_t kind)
{
    return kind == DMV_OPERAND_V16 || kind == DMV_OPERAND_V32 ||
           kind == DMV_OPERAND_COLOR || kind == DMV_OPERAND_STR;
}

/* Expected type of the destination variable of a variable instruction. */
static int dest_type(uint8_t opcode)
{
    switch (opcode)
    {
        case DMV_OP_SET:    return -1;              /* Either */
        case DMV_OP_FORMAT: return DMV_VAR_STR;
        default:            return DMV_VAR_INT;
    }
}

static bool operand_valid(const validator_t *v, uint8_t opcode, const dmv_opcode_info_t *info,
                          const uint8_t *insn, const dmv_layout_t *layout, uint8_t i)
{
    uint8_t  kind     = info->operands[i];
    const uint8_t *p  = insn + layout->offsets[i];
    uint32_t value    = (dmv_operand_size(kind) == 4) ? rd32(p) : rd16(p);
    bool     variable = (insn[2] & (1U << i)) != 0;

    if (variable)
    {
        if (kind == DMV_OPERAND_COLOR && (insn[3] & DMV_PAINT_GRADIENT) != 0)
            return false;               /* A gradient is never a variable */
        int type = var_type(v, value);
        /* SET copies between variables of the same type */
        if (opcode == DMV_OP_SET && i == 1)
            return type >= 0 && type == var_type(v, rd16(insn + layout->offsets[0]));
        return type == ((kind == DMV_OPERAND_STR) ? DMV_VAR_STR : DMV_VAR_INT);
    }

    switch (kind)
    {
        case DMV_OPERAND_V16:
            return true;
        case DMV_OPERAND_COLOR:
            return (insn[3] & DMV_PAINT_GRADIENT) == 0 || value < v->h.gradients.count;
        case DMV_OPERAND_V32:
            /* SET into a string variable: the immediate is a string index */
            if (opcode == DMV_OP_SET && i == 1 && var_type(v, rd16(insn + layout->offsets[0])) == DMV_VAR_STR)
                return string_valid(v, value);
            return true;
        case DMV_OPERAND_STR:
            return string_valid(v, value);
        case DMV_OPERAND_VAR:
        {
            int expected = dest_type(opcode);
            int type = (value < v->h.vars.count) ? var_type(v, value) : -1;
            return type >= 0 && (expected < 0 || type == expected);
        }
        case DMV_OPERAND_LABEL:
            return is_boundary(v, value);
        case DMV_OPERAND_BOX:
            return value < v->h.boxes.count || (opcode == DMV_OP_REDRAW && value == DMV_NONE);
        case DMV_OPERAND_FONT:
            return value < v->h.fonts.count;
        case DMV_OPERAND_EVENT:
            return value < DMV_EVENT_COUNT;
        default:
            return false;
    }
}

static bool read_box(validator_t *v, uint32_t index, uint8_t box[sizeof(dmv_box_t)])
{
    return rd(v, v->h.boxes.offset + index * (uint32_t)sizeof(dmv_box_t), box, sizeof(dmv_box_t));
}

/* Second pass: operands, and BOX / END against the box table. */
static dmv_status_t check_code(validator_t *v)
{
    uint32_t code_size = v->h.code.count * DMV_CODE_WORD;
    uint16_t stack[MAX_BOX_DEPTH];
    uint32_t depth = 0, boxes_seen = 0;
    uint8_t insn[MAX_INSTRUCTION], box[sizeof(dmv_box_t)];
    int previous = -1;                  /* Opcode of the instruction before */

    for (uint32_t pos = 0; pos < code_size; pos += insn[1])
    {
        uint32_t at = v->h.code.offset + pos;
        if (!rd(v, at, insn, 2))
            return fail(v, DMV_ERR_SIZE, at);
        if (insn[1] > sizeof(insn) || !rd(v, at, insn, insn[1]))
            return fail(v, DMV_ERR_SIZE, at);

        const dmv_opcode_info_t *info = dmv_get_opcode_info(insn[0]);
        dmv_layout_t layout;
        (void)dmv_get_layout(insn[0], &layout);

        /* Varmask bits only for value operands that exist */
        for (uint8_t i = 0; i < DMV_MAX_OPERANDS; i++)
        {
            if ((insn[2] & (1U << i)) && (i >= info->operand_count || !is_value_kind(info->operands[i])))
                return fail(v, DMV_ERR_OPERAND, at);
        }
        uint8_t flags = insn[3];
        if (color_operand(info) >= 0)
            flags &= (uint8_t)~DMV_PAINT_GRADIENT;
        if (!flags_valid(info->flags_kind, flags))
            return fail(v, DMV_ERR_OPERAND, at);
        for (uint8_t i = 0; i < info->operand_count; i++)
        {
            if (!operand_valid(v, insn[0], info, insn, &layout, i))
                return fail(v, (info->operands[i] == DMV_OPERAND_LABEL) ? DMV_ERR_LABEL : DMV_ERR_OPERAND, at);
        }

        uint32_t word = pos / DMV_CODE_WORD;
        if (insn[0] == DMV_OP_BOX)
        {
            uint16_t index = rd16(insn + layout.offsets[0]);
            uint16_t parent = depth ? stack[depth - 1] : (uint16_t)DMV_NONE;
            if (depth == MAX_BOX_DEPTH || !read_box(v, index, box) || rd16(box + 4) != word || rd16(box + 2) != parent)
                return fail(v, DMV_ERR_NESTING, at);
            stack[depth++] = index;
            boxes_seen++;
        }
        else if (insn[0] == DMV_OP_END)
        {
            if (depth == 0 || !read_box(v, stack[depth - 1], box) || rd16(box + 6) != word)
                return fail(v, DMV_ERR_NESTING, at);
            depth--;
        }
        else if (insn[0] == DMV_OP_ON && depth == 0)
        {
            return fail(v, DMV_ERR_NESTING, at);
        }
        else if (insn[0] == DMV_OP_OPACITY && previous != DMV_OP_BOX && previous != DMV_OP_SCROLL &&
                 previous != DMV_OP_FOCUS)
        {
            return fail(v, DMV_ERR_NESTING, at);       /* Directly after BOX (SCROLL, FOCUS) */
        }
        previous = insn[0];
    }
    if (depth != 0 || boxes_seen != v->h.boxes.count)
        return fail(v, DMV_ERR_NESTING, v->h.boxes.offset);
    return DMV_VALID;
}

static dmv_status_t check_boxes(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.boxes.count; i++)
    {
        uint8_t box[sizeof(dmv_box_t)];
        uint32_t at = v->h.boxes.offset + i * (uint32_t)sizeof(dmv_box_t);
        if (!read_box(v, i, box))
            return fail(v, DMV_ERR_TABLE, at);
        uint16_t parent = rd16(box + 2);
        if (!string_valid(v, rd16(box)) || (parent != DMV_NONE && parent >= v->h.boxes.count) ||
            !is_boundary(v, rd16(box + 4)) || !is_boundary(v, rd16(box + 6)))
            return fail(v, DMV_ERR_TABLE, at);
    }
    return DMV_VALID;
}

static dmv_status_t check_items(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.items.count; i++)
    {
        uint8_t p[sizeof(dmv_item_t)];
        uint32_t at = v->h.items.offset + i * (uint32_t)sizeof(dmv_item_t);
        if (!rd(v, at, p, sizeof(p)))
            return fail(v, DMV_ERR_TABLE, at);

        uint8_t kind = p[0], arg = p[1];
        uint16_t label = rd16(p + 2);
        uint32_t value = rd32(p + 4);
        bool ok;
        switch (kind)
        {
            case DMV_ITEM_INIT:   ok = arg == 0 && value == 0 && is_boundary(v, label); break;
            case DMV_ITEM_TIMER:  ok = arg == 0 && value > 0 && is_boundary(v, label); break;
            case DMV_ITEM_KEY:    ok = arg == 0 && value < 32 && is_boundary(v, label); break;
            case DMV_ITEM_NAVKEY: ok = arg < DMV_NAV_COUNT && value < 32 && label == DMV_NONE; break;
            default:              ok = false; break;
        }
        if (!ok)
            return fail(v, DMV_ERR_TABLE, at);
    }
    return DMV_VALID;
}

static dmv_status_t check_symbols(validator_t *v)
{
    for (uint32_t i = 0; i < v->h.symbols.count; i++)
    {
        uint8_t p[sizeof(dmv_symbol_t)];
        uint32_t at = v->h.symbols.offset + i * (uint32_t)sizeof(dmv_symbol_t);
        if (!rd(v, at, p, sizeof(p)))
            return fail(v, DMV_ERR_TABLE, at);
        uint16_t offset = rd16(p + 2);
        if (!string_valid(v, rd16(p)) || (offset != v->h.code.count && !is_boundary(v, offset)))
            return fail(v, DMV_ERR_TABLE, at);
    }
    return DMV_VALID;
}

static dmv_status_t check_header(validator_t *v)
{
    const dmv_header_t *h = &v->h;
    if (h->magic[0] != DMV_MAGIC_0 || h->magic[1] != DMV_MAGIC_1 ||
        h->magic[2] != DMV_MAGIC_2 || h->magic[3] != DMV_MAGIC_3 || h->file_size != v->in->size)
        return fail(v, DMV_ERR_HEADER, 0);
    if (h->version_major != DMV_VERSION_MAJOR || h->version_minor > DMV_VERSION_MINOR)
        return fail(v, DMV_ERR_VERSION, 4);
    if (!section_fits(v, &h->code, DMV_CODE_WORD) || h->code.count == 0 || h->code.count > 0xFFFFu ||
        !section_fits(v, &h->strings, 4) || h->strings.count > 0xFFFFu ||
        !section_fits(v, &h->vars, sizeof(dmv_var_t)) || h->vars.count > DMV_BUILTIN_BASE ||
        !section_fits(v, &h->fonts, sizeof(dmv_font_t)) || h->fonts.count > 0xFFFFu ||
        !section_fits(v, &h->boxes, sizeof(dmv_box_t)) || h->boxes.count >= DMV_NONE ||
        !section_fits(v, &h->items, sizeof(dmv_item_t)) ||
        !section_fits(v, &h->symbols, sizeof(dmv_symbol_t)) ||
        !section_fits(v, &h->gradients, sizeof(dmv_gradient_t)) || h->gradients.count > 0xFFFFu ||
        !section_fits(v, &h->stops, sizeof(dmv_stop_t)) || h->stops.count > 0xFFFFu)
        return fail(v, DMV_ERR_SECTION, 24);
    if (!string_valid(v, h->name))
        return fail(v, DMV_ERR_TABLE, 16);
    return DMV_VALID;
}

static void *alloc_zeroed(size_t size)
{
    void *p = Dmod_Malloc(size);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

dmv_status_t dmv_validate(const dmv_input_t *input, uint32_t *error_offset)
{
    validator_t v;
    uint8_t header[DMV_HEADER_SIZE];
    dmv_status_t status;

    memset(&v, 0, sizeof(v));
    v.in = input;
    if (input == NULL || input->read == NULL)
        status = DMV_ERR_ARGUMENT;
    else if (!rd(&v, 0, header, DMV_HEADER_SIZE_0_1) ||
             (rd16(header + 6) >= 2 && !rd(&v, DMV_HEADER_SIZE_0_1, header + DMV_HEADER_SIZE_0_1,
                                           DMV_HEADER_SIZE - DMV_HEADER_SIZE_0_1)))
        status = fail(&v, DMV_ERR_HEADER, 0);
    else
    {
        v.header_size = (rd16(header + 6) >= 2) ? DMV_HEADER_SIZE : DMV_HEADER_SIZE_0_1;
        read_header(header, &v.h);
        status = check_header(&v);
        if (status == DMV_VALID)
        {
            v.boundaries = alloc_zeroed(v.h.code.count / 8U + 1U);
            v.var_types = alloc_zeroed(v.h.vars.count + 1U);
            if (v.boundaries == NULL || v.var_types == NULL)
                status = DMV_ERR_MEMORY;
        }
        if (status == DMV_VALID)
            status = check_strings(&v);
        if (status == DMV_VALID)
            status = check_vars(&v);
        if (status == DMV_VALID)
            status = check_fonts(&v);
        if (status == DMV_VALID)
            status = check_gradients(&v);
        if (status == DMV_VALID)
            status = mark_instructions(&v);
        if (status == DMV_VALID && !is_boundary(&v, v.h.entry))
            status = fail(&v, DMV_ERR_LABEL, 18);
        if (status == DMV_VALID)
            status = check_boxes(&v);
        if (status == DMV_VALID)
            status = check_code(&v);
        if (status == DMV_VALID)
            status = check_items(&v);
        if (status == DMV_VALID)
            status = check_symbols(&v);
        if (v.boundaries != NULL)
            Dmod_Free(v.boundaries);
        if (v.var_types != NULL)
            Dmod_Free(v.var_types);
    }

    if (error_offset != NULL)
        *error_offset = v.error_offset;
    return status;
}
