#define DMOD_ENABLE_REGISTRATION ON
#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Loading a view: it is validated once (reading the file in pieces), then
 * its code and tables are read into memory - they are executed on every
 * redraw, and a validated view needs no checks while it runs.
 */

#define STRING_CHUNK    32u

/* ---- Reading ---- */

static bool rd(const dmv_input_t* in, uint32_t offset, void* buffer, size_t size)
{
    return size == 0 || in->read(in->ctx, offset, buffer, size) == 0;
}

static void* read_block(const dmv_input_t* in, uint32_t offset, size_t size, int* status)
{
    void* p = Dmod_Malloc(size + 1U);       /* +1: never a zero-size allocation */
    if (p == NULL)
    {
        *status = -ENOMEM;
        return NULL;
    }
    if (!rd(in, offset, p, size))
    {
        Dmod_Free(p);
        *status = -EIO;
        return NULL;
    }
    return p;
}

/* The string table (offsets, then the texts) - its end is the end of the
 * text the largest offset points at. */
static uint8_t* read_strings(const dmv_input_t* in, uint32_t at, uint32_t count, int* status)
{
    uint32_t max_offset = count * 4U;
    uint8_t entry[4];
    for (uint32_t i = 0; i < count; i++)
    {
        if (!rd(in, at + i * 4U, entry, sizeof(entry)))
        {
            *status = -EIO;
            return NULL;
        }
        if (rd32(entry) > max_offset)
            max_offset = rd32(entry);
    }

    uint32_t end = max_offset;
    for (bool terminated = (count == 0); !terminated; )
    {
        char chunk[STRING_CHUNK];
        uint32_t n = (in->size - (at + end) < STRING_CHUNK) ? in->size - (at + end) : STRING_CHUNK;
        if (n == 0 || !rd(in, at + end, chunk, n))
        {
            *status = -EIO;
            return NULL;
        }
        for (uint32_t k = 0; k < n && !terminated; k++, end++)
            terminated = chunk[k] == '\0';
    }
    return read_block(in, at, end, status);
}

const char* view_string(const struct libdmview* v, uint32_t index)
{
    return (const char*)v->strings + rd32(v->strings + index * 4U);
}

/* ---- Environment (env: variables) ---- */

/* Decimal with an optional '-', or 0x hexadecimal */
static bool parse_int(const char* s, int32_t* value)
{
    bool negative = (*s == '-');
    uint32_t v = 0, base = 10;
    if (negative)
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        s += 2;
    }
    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++)
    {
        uint32_t d = (*s >= '0' && *s <= '9') ? (uint32_t)(*s - '0')
                   : (*s >= 'a' && *s <= 'f') ? (uint32_t)(*s - 'a' + 10)
                   : (*s >= 'A' && *s <= 'F') ? (uint32_t)(*s - 'A' + 10) : 99u;
        if (d >= base)
            return false;
        v = v * base + d;
    }
    *value = negative ? -(int32_t)v : (int32_t)v;
    return true;
}

static void env_write_int(const struct libdmview* v, uint32_t index)
{
    char text[16];
    Dmod_SnPrintf(text, sizeof(text), "%d", (int)v->ints[index]);
    (void)Dmod_SetEnv(view_string(v, v->vars[index].env), text, 1);
}

/* ---- Dependencies ---- */

void view_mark_dirty(struct libdmview* v, int32_t box)
{
    if (box == ROOT)
        v->full = true;
    else
    {
        v->boxes[box].flags |= BOXF_DIRTY;
        v->any_dirty = true;
    }
}

void view_invalidate_deps(struct libdmview* v, uint32_t word, uint32_t bit)
{
    uint32_t* row = v->deps + word;
    for (uint32_t b = 0; b < v->box_count; b++, row += v->dep_words)
    {
        if ((*row & bit) != 0)
            view_mark_dirty(v, (int32_t)b);
    }
    if ((*row & bit) != 0)          /* The root's row comes last */
        v->full = true;
}

void view_set_int(struct libdmview* v, uint32_t index, int32_t value)
{
    if (v->ints[index] == value)
        return;
    v->ints[index] = value;
    /* Writes during a draw are temporaries - they invalidate nothing */
    if (!v->drawing)
        view_invalidate_deps(v, index / 32U, 1U << (index % 32U));
    if ((v->vars[index].flags & DMV_VARF_ENV) != 0)
        env_write_int(v, index);
}

void view_set_string(struct libdmview* v, uint32_t index, const char* value)
{
    char* dst = v->strs[index];
    size_t len = strlen(value);
    if (len > v->vars[index].capacity)
        len = v->vars[index].capacity;
    if (strncmp(dst, value, len) == 0 && dst[len] == '\0')
        return;
    memcpy(dst, value, len);
    dst[len] = '\0';
    if (!v->drawing)
        view_invalidate_deps(v, index / 32U, 1U << (index % 32U));
    if ((v->vars[index].flags & DMV_VARF_ENV) != 0)
        (void)Dmod_SetEnv(view_string(v, v->vars[index].env), dst, 1);
}

/* The string after what the variable holds - as much of it as its size takes */
void view_append_string(struct libdmview* v, uint32_t index, const char* tail)
{
    char* dst = v->strs[index];
    size_t n = strlen(dst), room = v->vars[index].capacity;
    size_t m = strlen(tail);
    if (n >= room || m == 0)
        return;
    if (m > room - n)
        m = room - n;
    if (tail >= dst && tail <= dst + room)
    {
        /* Appended to itself: copy it from before it grows */
        memmove(dst + n, tail, m);
    }
    else
        memcpy(dst + n, tail, m);
    dst[n + m] = '\0';
    if (!v->drawing)
        view_invalidate_deps(v, index / 32U, 1U << (index % 32U));
    if ((v->vars[index].flags & DMV_VARF_ENV) != 0)
        (void)Dmod_SetEnv(view_string(v, v->vars[index].env), dst, 1);
}

/* ---- Loading ---- */

static void free_view(struct libdmview* v)
{
    if (v->strs != NULL)
    {
        for (uint32_t i = 0; i < v->var_count; i++)
        {
            if (v->strs[i] != NULL)
                Dmod_Free(v->strs[i]);
        }
    }
    if (v->fonts != NULL)
    {
        for (uint32_t i = 0; i < v->font_count; i++)
            font_release(&v->fonts[i]);
    }
    images_free_slots(v);
    if (v->gradients != NULL)
    {
        for (uint32_t i = 0; i < v->gradient_count; i++)
        {
            if (v->gradients[i].dither != NULL)
                Dmod_Free(v->gradients[i].dither);
        }
    }
    void* blocks[] = { v->code, v->strings, v->vars, v->fonts, v->boxes, v->items, v->gradients, v->stops,
                       v->ints, v->strs, v->deps, v->goto_path, v->goto_taken, v->dir };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
    {
        if (blocks[i] != NULL)
            Dmod_Free(blocks[i]);
    }
    v->magic = 0;
    Dmod_Free(v);
}

static int load_tables(struct libdmview* v, const dmv_input_t* in, const uint8_t* h)
{
    int status = 0;
    uint32_t code_at = rd32(h + 24), strings_at = rd32(h + 32), vars_at = rd32(h + 40), fonts_at = rd32(h + 48);
    uint32_t boxes_at = rd32(h + 56), items_at = rd32(h + 64);

    v->code_size = rd32(h + 28) * DMV_CODE_WORD;
    v->string_count = rd32(h + 36);
    v->var_count = rd32(h + 44);
    v->font_count = rd32(h + 52);
    v->box_count = rd32(h + 60);
    v->item_count = rd32(h + 68);
    v->width = rd16(h + 12);
    v->height = rd16(h + 14);
    v->entry = rd16(h + 18);
    v->longpress_ms = rd16(h + 20);

    if ((v->code = read_block(in, code_at, v->code_size, &status)) == NULL ||
        (v->strings = read_strings(in, strings_at, v->string_count, &status)) == NULL)
        return status;

    /* Variables */
    uint8_t* raw = read_block(in, vars_at, v->var_count * sizeof(dmv_var_t), &status);
    v->vars = Dmod_Malloc(v->var_count * sizeof(dmv_var_t) + 1U);
    v->ints = Dmod_Malloc(v->var_count * sizeof(int32_t) + 1U);
    v->strs = Dmod_Malloc(v->var_count * sizeof(char*) + 1U);
    if (raw == NULL || v->vars == NULL || v->ints == NULL || v->strs == NULL)
    {
        if (raw != NULL)
            Dmod_Free(raw);
        return (status != 0) ? status : -ENOMEM;
    }
    memset(v->strs, 0, v->var_count * sizeof(char*));
    for (uint32_t i = 0; i < v->var_count; i++)
    {
        const uint8_t* p = raw + i * sizeof(dmv_var_t);
        dmv_var_t* var = &v->vars[i];
        var->type = p[0];
        var->flags = p[1];
        var->capacity = rd16(p + 2);
        var->name = rd16(p + 4);
        var->env = rd16(p + 6);
        var->init = (int32_t)rd32(p + 8);
        v->ints[i] = 0;
        if (var->type == DMV_VAR_STR)
        {
            if ((v->strs[i] = Dmod_Malloc((size_t)var->capacity + 1U)) == NULL)
            {
                Dmod_Free(raw);
                return -ENOMEM;
            }
            strncpy(v->strs[i], view_string(v, (uint32_t)var->init), var->capacity);
            v->strs[i][var->capacity] = '\0';
        }
        else
            v->ints[i] = var->init;

        /* env: variables take over the system's value */
        if ((var->flags & DMV_VARF_ENV) != 0)
        {
            const char* value = Dmod_GetEnv(view_string(v, var->env));
            int32_t n;
            if (value != NULL && var->type == DMV_VAR_STR)
            {
                strncpy(v->strs[i], value, var->capacity);
                v->strs[i][var->capacity] = '\0';
            }
            else if (value != NULL && parse_int(value, &n))
                v->ints[i] = n;
        }
    }
    Dmod_Free(raw);

    /* Fonts: each spec's font file, or the built-in font */
    if ((raw = read_block(in, fonts_at, v->font_count * sizeof(dmv_font_t), &status)) == NULL)
        return status;
    v->fonts = Dmod_Malloc(v->font_count * sizeof(font_t) + 1U);
    if (v->fonts == NULL)
    {
        Dmod_Free(raw);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < v->font_count; i++)
        font_resolve(view_string(v, rd16(raw + i * sizeof(dmv_font_t) + 2U)), v->dir, &v->fonts[i]);
    Dmod_Free(raw);

    /* Boxes */
    if ((raw = read_block(in, boxes_at, v->box_count * sizeof(dmv_box_t), &status)) == NULL)
        return status;
    v->boxes = Dmod_Malloc(v->box_count * sizeof(rbox_t) + 1U);
    if (v->boxes == NULL)
    {
        Dmod_Free(raw);
        return -ENOMEM;
    }
    memset(v->boxes, 0, v->box_count * sizeof(rbox_t));
    for (uint32_t i = 0; i < v->box_count; i++)
        v->boxes[i].seen = 255u;                    /* Seen until a draw says otherwise */
    for (uint32_t i = 0; i < v->box_count; i++)
    {
        const uint8_t* p = raw + i * sizeof(dmv_box_t);
        rbox_t* b = &v->boxes[i];
        uint16_t parent = rd16(p + 2);
        b->parent = (parent == DMV_NONE) ? ROOT : (int16_t)parent;
        b->begin = rd16(p + 4);
        b->end = rd16(p + 6);
        const uint8_t* insn = v->code + b->begin * DMV_CODE_WORD;
        b->flags = insn[3] & DMV_BOX_FLAGS_MASK;
        if ((insn[2] & 0x1Eu) != 0)                 /* x, y, w or h is a variable */
            b->flags |= BOXF_GEOMETRY_VAR;
        for (uint32_t e = 0; e < DMV_EVENT_COUNT; e++)
            b->handlers[e] = DMV_NONE;

        /* OPACITY after BOX (SCROLL, FOCUS) - from a variable, or below opaque */
        const uint8_t* next = insn + insn[1];
        while (next[0] == DMV_OP_SCROLL || next[0] == DMV_OP_FOCUS)
            next += next[1];
        if (next[0] == DMV_OP_OPACITY && ((next[2] & 0x01u) != 0 || (int16_t)rd16(next + 4) < DMV_OPACITY_MAX))
            b->flags |= BOXF_TRANSLUCENT;
    }
    Dmod_Free(raw);

    /* What is drawn in a translucent box shows what lies beneath it: none of
     * its boxes covers itself, a redraw starts beneath them */
    for (uint32_t i = 0; i < v->box_count; i++)
    {
        for (int32_t p = (int32_t)i; p != ROOT; p = v->boxes[p].parent)
        {
            if ((v->boxes[p].flags & BOXF_TRANSLUCENT) != 0)
            {
                v->boxes[i].flags = (uint8_t)((v->boxes[i].flags | BOXF_TRANSLUCENT) & ~DMV_BOX_OPAQUE);
                break;
            }
        }
    }

    /* View-level items */
    if ((raw = read_block(in, items_at, v->item_count * sizeof(dmv_item_t), &status)) == NULL)
        return status;
    v->items = Dmod_Malloc(v->item_count * sizeof(item_t) + 1U);
    if (v->items == NULL)
    {
        Dmod_Free(raw);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < v->item_count; i++)
    {
        const uint8_t* p = raw + i * sizeof(dmv_item_t);
        v->items[i].kind = p[0];
        v->items[i].arg = p[1];
        v->items[i].label = rd16(p + 2);
        v->items[i].value = rd32(p + 4);
        v->items[i].due = (v->items[i].kind == DMV_ITEM_TIMER) ? v->items[i].value : 0;
    }
    Dmod_Free(raw);

    /* Gradients (version 0.2) - their palettes are built when first drawn */
    if (rd16(h + 6) >= 2)
    {
        v->gradient_count = rd32(h + 84);
        uint32_t stop_count = rd32(h + 92);
        if ((raw = read_block(in, rd32(h + 80), v->gradient_count * sizeof(dmv_gradient_t), &status)) == NULL)
            return status;
        v->gradients = Dmod_Malloc(v->gradient_count * sizeof(grad_t) + 1U);
        v->stops = Dmod_Malloc(stop_count * sizeof(dmv_stop_t) + 1U);
        uint8_t* stops = read_block(in, rd32(h + 88), stop_count * sizeof(dmv_stop_t), &status);
        if (v->gradients == NULL || v->stops == NULL || stops == NULL)
        {
            Dmod_Free(raw);
            if (stops != NULL)
                Dmod_Free(stops);
            return (status != 0) ? status : -ENOMEM;
        }
        for (uint32_t i = 0; i < stop_count; i++)
        {
            v->stops[i].color = rd32(stops + i * sizeof(dmv_stop_t));
            v->stops[i].position = rd16(stops + i * sizeof(dmv_stop_t) + 4U);
            v->stops[i].reserved = 0;
        }
        Dmod_Free(stops);
        for (uint32_t i = 0; i < v->gradient_count; i++)
        {
            const uint8_t* p = raw + i * sizeof(dmv_gradient_t);
            grad_t* g = &v->gradients[i];
            g->kind = p[2];
            g->count = p[3];
            g->first = rd16(p + 4);
            for (uint32_t k = 0; k < 4U; k++)
                g->param[k] = (int16_t)rd16(p + 6U + 2U * k);
            g->opaque = true;
            for (uint32_t k = 0; k < g->count; k++)
                g->opaque = g->opaque && (v->stops[g->first + k].color >> 24) == 0xFFu;
            g->lut_format = GRADIENT_NO_LUT;
            g->dither = NULL;
        }
        Dmod_Free(raw);
    }

    /* Dependencies: a row per box and one for the root */
    v->dep_words = (v->var_count + 31U) / 32U + 1U;
    size_t deps_size = (v->box_count + 1U) * v->dep_words * sizeof(uint32_t);
    if ((v->deps = Dmod_Malloc(deps_size)) == NULL)
        return -ENOMEM;
    memset(v->deps, 0, deps_size);
    return (images_slots(v) == 0) ? 0 : -ENOMEM;
}

/* A view from `input`; `dir` (allocated, may be NULL) is the directory of
 * its file - relative image and font paths start there. It is the view's
 * from now on, or freed. */
static libdmview_t open_view(const dmv_input_t* input, char* dir, int* status)
{
    uint8_t header[DMV_HEADER_SIZE];
    int ret = 0;

    if (status != NULL)
        *status = 0;
    if (input == NULL || input->read == NULL)
    {
        if (dir != NULL)
            Dmod_Free(dir);
        if (status != NULL)
            *status = -EINVAL;
        return NULL;
    }
    switch (dmv_validate(input, NULL))
    {
        case DMV_VALID:      break;
        case DMV_ERR_IO:     ret = -EIO; break;
        case DMV_ERR_MEMORY: ret = -ENOMEM; break;
        default:             ret = -EBADMSG; break;
    }

    struct libdmview* v = NULL;
    if (ret == 0 && (!rd(input, 0, header, DMV_HEADER_SIZE_0_1) ||
                     (rd16(header + 6) >= 2 && !rd(input, DMV_HEADER_SIZE_0_1, header + DMV_HEADER_SIZE_0_1,
                                                   DMV_HEADER_SIZE - DMV_HEADER_SIZE_0_1))))
        ret = -EIO;
    if (ret == 0 && (v = Dmod_Malloc(sizeof(*v))) == NULL)
        ret = -ENOMEM;
    if (ret == 0)
    {
        memset(v, 0, sizeof(*v));
        v->magic = VIEW_MAGIC;
        v->captured = ROOT;
        v->occluder = -1;
        v->full = true;
        v->dir = dir;
        dir = NULL;
        ret = load_tables(v, input, header);
    }
    if (dir != NULL)
        Dmod_Free(dir);
    if (ret != 0)
    {
        if (v != NULL)
            free_view(v);
        if (status != NULL)
            *status = ret;
        return NULL;
    }

    /* .init handlers, before the first draw */
    for (uint32_t i = 0; i < v->item_count; i++)
    {
        if (v->items[i].kind == DMV_ITEM_INIT)
            exec_handler(v, ROOT, v->items[i].label);
    }
    return v;
}

dmod_libdmview_api_declaration(1.0, libdmview_t, _open_input, ( const dmv_input_t* input, int* status ))
{
    return open_view(input, NULL, status);
}

/* static: its address is handed out as a callback - a global function's
 * address would be taken through the GOT, which the dmod loader does not
 * relocate */
static int file_read(void* ctx, uint32_t offset, void* buffer, size_t size)
{
    if (Dmod_FileSeek(ctx, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) != 0)
        return -EIO;
    return (Dmod_FileRead(buffer, 1, size, ctx) == size) ? 0 : -EIO;
}

dmod_libdmview_api_declaration(1.0, libdmview_t, _open, ( const char* path, int* status ))
{
    size_t size = 0;
    if (path == NULL)
    {
        if (status != NULL)
            *status = -EINVAL;
        return NULL;
    }
    void* file = Dmod_FileOpen(path, "rb");
    if (file == NULL || !Dmod_FileSizeToSizeT(Dmod_FileSize(file), &size) || size > 0xFFFFFFFFu)
    {
        if (file != NULL)
            Dmod_FileClose(file);
        if (status != NULL)
            *status = -ENOENT;
        return NULL;
    }
    dmv_input_t input;
    input.read = file_read;
    input.ctx = file;
    input.size = (uint32_t)size;

    /* Relative image and font paths start in the view's directory */
    const char* slash = strrchr(path, '/');
    char* dir = NULL;
    if (slash != NULL && (dir = Dmod_Malloc((size_t)(slash - path) + 1U)) != NULL)
    {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = '\0';
    }
    libdmview_t v = open_view(&input, dir, status);
    Dmod_FileClose(file);
    return v;
}

static bool is_view(libdmview_t v)
{
    return v != NULL && v->magic == VIEW_MAGIC;
}

dmod_libdmview_api_declaration(1.0, void, _close, ( libdmview_t view ))
{
    if (is_view(view))
        free_view(view);
}

dmod_libdmview_api_declaration(1.0, int, _get_size, ( libdmview_t view, uint16_t* width, uint16_t* height ))
{
    if (!is_view(view))
        return -EINVAL;
    if (width != NULL)
        *width = view->width;
    if (height != NULL)
        *height = view->height;
    return 0;
}

dmod_libdmview_api_declaration(1.0, void, _invalidate, ( libdmview_t view ))
{
    if (is_view(view))
        view->full = true;
}

dmod_libdmview_api_declaration(1.0, const char*, _take_goto, ( libdmview_t view ))
{
    if (!is_view(view) || view->goto_path == NULL)
        return NULL;
    if (view->goto_taken != NULL)
        Dmod_Free(view->goto_taken);
    view->goto_taken = view->goto_path;
    view->goto_path = NULL;
    return view->goto_taken;
}

/* ---- Variables by name ---- */

static int find_var(libdmview_t v, const char* name, uint8_t type)
{
    if (!is_view(v) || name == NULL)
        return -EINVAL;
    for (uint32_t i = 0; i < v->var_count; i++)
    {
        if (strcmp(view_string(v, v->vars[i].name), name) == 0)
            return (v->vars[i].type == type) ? (int)i : -EINVAL;
    }
    return -ENOENT;
}

dmod_libdmview_api_declaration(1.0, int, _get_int, ( libdmview_t view, const char* name, int32_t* value ))
{
    int i = find_var(view, name, DMV_VAR_INT);
    if (i < 0 || value == NULL)
        return (i < 0) ? i : -EINVAL;
    *value = view->ints[i];
    return 0;
}

dmod_libdmview_api_declaration(1.0, int, _set_int, ( libdmview_t view, const char* name, int32_t value ))
{
    int i = find_var(view, name, DMV_VAR_INT);
    if (i < 0)
        return i;
    view_set_int(view, (uint32_t)i, value);
    return 0;
}

dmod_libdmview_api_declaration(1.0, int, _set_string, ( libdmview_t view, const char* name, const char* value ))
{
    int i = find_var(view, name, DMV_VAR_STR);
    if (i < 0 || value == NULL)
        return (i < 0) ? i : -EINVAL;
    view_set_string(view, (uint32_t)i, value);
    return 0;
}

dmod_libdmview_api_declaration(1.0, dmv_status_t, _validate, ( const dmv_input_t* input, uint32_t* error_offset ))
{
    return dmv_validate(input, error_offset);
}

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    int ret = claims_init();
    if (ret == 0)
        ret = fonts_init();
    return (ret == 0) ? images_init() : ret;
}

int dmod_deinit(void)
{
    claims_deinit();
    fonts_deinit();
    images_deinit();
    return 0;
}
