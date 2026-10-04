#include "private.h"
#include "dmosi.h"
#include <string.h>

/*
 * Font files (.dmvf, docs/font-format.md): antialiased glyphs, 4 bits of
 * coverage per pixel. A file is read into memory once, checked, and shared
 * by every view that names it - several views on several displays use the
 * same fonts. A font spec resolves to:
 *
 *  - "builtin-N": the built-in 8x8 font, magnified by N / 8 - never a file
 *    (a console's fixed-width font);
 *  - a path, when the spec has a '/' - a relative one starts in the view's
 *    directory, like an image's;
 *  - else <view's directory>/<spec>.dmvf - fonts installed next to a view -
 *    and when there is none $DMVIEW_FONTS/<spec>.dmvf;
 *  - the built-in font, when there is no such file or it is not valid.
 */

#define FONT_SUFFIX     ".dmvf"
#define BUILTIN_PREFIX  "builtin"

static dmosi_mutex_t    g_fonts_lock;
static font_file_t*     g_fonts;

int fonts_init(void)
{
    g_fonts_lock = dmosi_mutex_create(false);
    return (g_fonts_lock != NULL) ? 0 : -1;
}

void fonts_deinit(void)
{
    while (g_fonts != NULL)
    {
        font_file_t* f = g_fonts;
        g_fonts = f->next;
        Dmod_Free(f->data);
        Dmod_Free(f->path);
        Dmod_Free(f);
    }
    if (g_fonts_lock != NULL)
        dmosi_mutex_destroy(g_fonts_lock);
    g_fonts_lock = NULL;
}

static inline uint16_t glyph_codepoint(const uint8_t* g)
{
    return rd16(g + 4);
}

/* The file's tables lie inside it, every bitmap inside the bitmaps, the
 * codepoints ascending. */
static bool check(font_file_t* f, uint32_t size)
{
    const uint8_t* h = f->data;
    if (size < sizeof(dmvf_header_t) || h[0] != DMVF_MAGIC_0 || h[1] != DMVF_MAGIC_1 || h[2] != DMVF_MAGIC_2 ||
        h[3] != DMVF_MAGIC_3 || rd16(h + 4) != DMVF_VERSION_MAJOR || rd32(h + 8) != size)
        return false;
    uint32_t count = rd32(h + 20), glyphs = rd32(h + 24), bitmaps = rd32(h + 28);
    if (count > 0xFFFFu || glyphs % 4U != 0 || glyphs < sizeof(dmvf_header_t) ||
        (uint64_t)glyphs + (uint64_t)count * sizeof(dmvf_glyph_t) > bitmaps || bitmaps > size)
        return false;

    f->glyphs = f->data + glyphs;
    f->bitmaps = f->data + bitmaps;
    f->count = count;
    f->line_height = rd16(h + 14);
    f->ascent = (int16_t)rd16(h + 16);
    uint32_t bitmaps_size = size - bitmaps;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* g = f->glyphs + i * sizeof(dmvf_glyph_t);
        uint32_t bytes = ((uint32_t)g[6] + 1U) / 2U * g[7];
        if ((uint64_t)rd32(g) + bytes > bitmaps_size || g[11] != 0 ||
            (i > 0 && glyph_codepoint(g) <= glyph_codepoint(g - sizeof(dmvf_glyph_t))))
            return false;
    }

    /* ASCII without a search */
    for (uint32_t c = 0; c < FONT_ASCII_COUNT; c++)
        f->ascii[c] = FONT_NO_GLYPH;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t c = glyph_codepoint(f->glyphs + i * sizeof(dmvf_glyph_t));
        if (c >= FONT_ASCII_FIRST && c < FONT_ASCII_FIRST + FONT_ASCII_COUNT)
            f->ascii[c - FONT_ASCII_FIRST] = (uint16_t)i;
    }
    f->fallback = f->ascii['?' - FONT_ASCII_FIRST];
    return true;
}

static font_file_t* load(const char* path)
{
    size_t size = 0;
    void* file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
        return NULL;
    font_file_t* f = NULL;
    if (Dmod_FileSizeToSizeT(Dmod_FileSize(file), &size) && size <= 0x7FFFFFFFu &&
        (f = Dmod_Malloc(sizeof(*f))) != NULL)
    {
        memset(f, 0, sizeof(*f));
        f->data = Dmod_Malloc(size + 1U);
        f->path = Dmod_StrDup(path);
        if (f->data == NULL || f->path == NULL || Dmod_FileRead(f->data, 1, size, file) != size ||
            !check(f, (uint32_t)size))
        {
            DMOD_LOG_WARN("libdmview: %s is not a usable font\n", path);
            if (f->data != NULL)
                Dmod_Free(f->data);
            if (f->path != NULL)
                Dmod_Free(f->path);
            Dmod_Free(f);
            f = NULL;
        }
    }
    Dmod_FileClose(file);
    return f;
}

static font_file_t* open_shared(const char* path)
{
    if (g_fonts_lock == NULL)
        return NULL;
    dmosi_mutex_lock(g_fonts_lock);
    font_file_t* f = g_fonts;
    while (f != NULL && strcmp(f->path, path) != 0)
        f = f->next;
    if (f == NULL && (f = load(path)) != NULL)
    {
        f->next = g_fonts;
        g_fonts = f;
    }
    if (f != NULL)
        f->refs++;
    dmosi_mutex_unlock(g_fonts_lock);
    return f;
}

void font_release(font_t* font)
{
    font_file_t* f = font->file;
    font->file = NULL;
    if (f == NULL || g_fonts_lock == NULL)
        return;
    dmosi_mutex_lock(g_fonts_lock);
    if (--f->refs == 0)
    {
        for (font_file_t** p = &g_fonts; *p != NULL; p = &(*p)->next)
        {
            if (*p == f)
            {
                *p = f->next;
                break;
            }
        }
        Dmod_Free(f->data);
        Dmod_Free(f->path);
        Dmod_Free(f);
    }
    dmosi_mutex_unlock(g_fonts_lock);
}

/* dir + '/' + name + suffix, allocated */
static char* join(const char* dir, const char* name, const char* suffix)
{
    size_t ld = strlen(dir), ln = strlen(name), ls = strlen(suffix);
    bool slash = ld > 0 && dir[ld - 1U] != '/';
    char* path = Dmod_Malloc(ld + (slash ? 1U : 0U) + ln + ls + 1U);
    if (path == NULL)
        return NULL;
    memcpy(path, dir, ld);
    if (slash)
        path[ld++] = '/';
    memcpy(path + ld, name, ln);
    memcpy(path + ld + ln, suffix, ls + 1U);
    return path;
}

/* The font file at dir/name+suffix, NULL when there is none */
static font_file_t* open_in(const char* dir, const char* name, const char* suffix)
{
    char* path = join(dir, name, suffix);
    if (path == NULL)
        return NULL;
    font_file_t* f = open_shared(path);
    Dmod_Free(path);
    return f;
}

void font_resolve(const char* spec, const char* dir, font_t* font)
{
    font->file = NULL;
    font->scale = font_scale_for(spec);
    if (strncmp(spec, BUILTIN_PREFIX, sizeof(BUILTIN_PREFIX) - 1U) == 0)
        return;

    if (strchr(spec, '/') != NULL)
    {
        font->file = (spec[0] != '/' && dir != NULL) ? open_in(dir, spec, "") : open_shared(spec);
        return;
    }
    if (dir != NULL && (font->file = open_in(dir, spec, FONT_SUFFIX)) != NULL)
        return;
    const char* fonts = Dmod_GetEnv("DMVIEW_FONTS");
    if (fonts != NULL && fonts[0] != '\0')
        font->file = open_in(fonts, spec, FONT_SUFFIX);
}

const uint8_t* font_glyph(const font_file_t* f, uint32_t codepoint)
{
    uint32_t index = FONT_NO_GLYPH;
    if (codepoint >= FONT_ASCII_FIRST && codepoint < FONT_ASCII_FIRST + FONT_ASCII_COUNT)
        index = f->ascii[codepoint - FONT_ASCII_FIRST];
    else if (codepoint <= 0xFFFFu)
    {
        uint32_t lo = 0, hi = f->count;
        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2U;
            uint32_t c = glyph_codepoint(f->glyphs + mid * sizeof(dmvf_glyph_t));
            if (c == codepoint)
            {
                index = mid;
                break;
            }
            if (c < codepoint)
                lo = mid + 1U;
            else
                hi = mid;
        }
    }
    if (index == FONT_NO_GLYPH)
        index = f->fallback;
    return (index == FONT_NO_GLYPH) ? NULL : f->glyphs + index * sizeof(dmvf_glyph_t);
}
