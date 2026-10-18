#include "private.h"
#include "dmosi.h"
#include <string.h>

/*
 * Images (.dmvi, docs/image-format.md): pixels in a few raw formats, drawn
 * as they are - libdmview decodes nothing. Other formats (PNG, JPEG, ...)
 * are converted into .dmvi files by todmvi, at build time or on a device
 * that has it.
 *
 * A file is read into memory once - unpacked with dmod's compression
 * (Dmod_Compression_Unpack) when it is compressed - checked, and shared by
 * every view that shows it. Each IMAGE / ICON instruction of a view has a slot: the path it
 * showed last and its image - a string variable set to another path loads
 * the new image on the next draw and releases the old one.
 *
 * What a view shows when it is shown is read at once, in the draw. An image
 * a slot is given later - a string variable set to another path - is read by
 * the loader thread instead: the draw shows nothing in its place, and once the
 * file is there (images_poll(), in libdmview_update()) the box it is in is
 * drawn again. A slow source - an SD card - never stops the drawing: a list
 * whose rows change their images keeps scrolling while they are read.
 *
 * A path starting with '/' is used as it is; any other is relative to the
 * directory of the view file.
 */

#define CHUNK   64          /* Pixels converted at a time */
#define HEADER  ((uint32_t)sizeof(dmvi_header_t))
#define LOADER_STACK        4096u
#define LOADER_WAIT_MS      1000
#define LOADER_WAKE_MAX     1024u

static dmosi_mutex_t    g_images_lock;
static image_t*         g_images;

/* The loader thread: reads the images that are IMAGE_LOADING */
static dmosi_thread_t   g_loader;
static dmosi_semaphore_t g_loader_wake;
static volatile bool    g_loader_quit;
static volatile uint32_t g_loaded;      /* Grows when an image is read (or failed): views look at their slots */

static void free_image(image_t* im)
{
    if (im->data != NULL)
        Dmod_Free(im->data);
    if (im->path != NULL)
        Dmod_Free(im->path);
    if (im->palette != NULL)
        Dmod_Free(im->palette);
    Dmod_Free(im);
}

int images_init(void)
{
    g_images_lock = dmosi_mutex_create(false);
    return (g_images_lock != NULL) ? 0 : -1;
}

void images_deinit(void)
{
    if (g_loader != NULL)
    {
        g_loader_quit = true;
        (void)dmosi_semaphore_post(g_loader_wake, 1);
        (void)dmosi_thread_join(g_loader);
        dmosi_thread_destroy(g_loader);
        g_loader = NULL;
    }
    if (g_loader_wake != NULL)
        dmosi_semaphore_destroy(g_loader_wake);
    g_loader_wake = NULL;
    g_loader_quit = false;
    while (g_images != NULL)
    {
        image_t* im = g_images;
        g_images = im->next;
        free_image(im);
    }
    if (g_images_lock != NULL)
        dmosi_mutex_destroy(g_images_lock);
    g_images_lock = NULL;
}

/* ---- Loading ---- */

/* `rows` rows of `row` bytes, `stride` apart from `at`, inside the file */
static bool fits(uint32_t at, uint32_t stride, uint32_t rows, uint32_t row, uint32_t size)
{
    return at >= sizeof(dmvi_header_t) && (uint64_t)at + (uint64_t)stride * (rows - 1U) + row <= size;
}

/* The header's tables lie inside the (unpacked) image of `size` bytes, the
 * fields a format does not use are 0. */
static bool check(image_t* im, uint32_t size)
{
    const uint8_t* h = im->data;
    uint32_t w = rd16(h + 12), rows = rd16(h + 14), format = h[16], count = rd16(h + 18);
    uint32_t stride = rd32(h + 20), pixels = rd32(h + 24), alpha_stride = rd32(h + 28), alpha = rd32(h + 32);
    uint32_t palette = rd32(h + 36);
    if (w == 0 || rows == 0 || h[17] != 0 || pixels % 4U != 0)
        return false;

    uint32_t row, align;
    switch (format)
    {
        case DMVI_FORMAT_RGB565:
        case DMVI_FORMAT_RGB565A8:  row = w * 2U; align = 2; break;
        case DMVI_FORMAT_ARGB8888:  row = w * 4U; align = 4; break;
        case DMVI_FORMAT_I8:
        case DMVI_FORMAT_A8:        row = w; align = 1; break;
        case DMVI_FORMAT_A4:        row = (w + 1U) / 2U; align = 1; break;
        default:                    return false;
    }
    if (stride < row || stride % align != 0 || !fits(pixels, stride, rows, row, size))
        return false;
    if (format == DMVI_FORMAT_RGB565A8 ? (alpha_stride < w || !fits(alpha, alpha_stride, rows, w, size))
                                       : (alpha_stride != 0 || alpha != 0))
        return false;
    if (format == DMVI_FORMAT_I8 ? (count == 0 || count > DMVI_PALETTE_MAX || palette % 4U != 0 ||
                                    !fits(palette, 0, 1, count * 4U, size))
                                 : (count != 0 || palette != 0))
        return false;

    im->format = (uint8_t)format;
    im->width = (uint16_t)w;
    im->height = (uint16_t)rows;
    im->stride = stride;
    im->pixels = im->data + pixels;
    im->alpha_stride = alpha_stride;
    im->alpha = (format == DMVI_FORMAT_RGB565A8) ? im->data + alpha : NULL;
    return true;
}

/* `count` bytes, `step` apart from `p`, all 0xFF */
static bool all_opaque(const uint8_t* p, uint32_t step, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++, p += step)
    {
        if (*p != 0xFFu)
            return false;
    }
    return true;
}

/* The palette of an I8 image, and whether any pixel is translucent - an
 * opaque image is copied, not blended. */
static bool prepare(image_t* im)
{
    switch (im->format)
    {
        case DMVI_FORMAT_RGB565:
            im->opaque = true;
            break;
        case DMVI_FORMAT_ARGB8888:
            im->opaque = true;
            for (uint32_t y = 0; y < im->height && im->opaque; y++)
                im->opaque = all_opaque(im->pixels + y * im->stride + 3U, 4U, im->width);
            break;
        case DMVI_FORMAT_RGB565A8:
            im->opaque = true;
            for (uint32_t y = 0; y < im->height && im->opaque; y++)
                im->opaque = all_opaque(im->alpha + y * im->alpha_stride, 1U, im->width);
            break;
        case DMVI_FORMAT_I8:
        {
            const uint8_t* h = im->data;
            uint32_t count = rd16(h + 18);
            const uint8_t* colors = im->data + rd32(h + 36);
            if ((im->palette = Dmod_Malloc(DMVI_PALETTE_MAX * sizeof(uint32_t))) == NULL)
                return false;
            memset(im->palette, 0, DMVI_PALETTE_MAX * sizeof(uint32_t));     /* Missing colors: transparent */
            im->opaque = true;
            for (uint32_t i = 0; i < count; i++)
            {
                im->palette[i] = rd32(colors + i * 4U);
                im->opaque = im->opaque && (im->palette[i] >> 24) == 0xFFu;
            }
            for (uint32_t y = 0; y < im->height && im->opaque; y++)
            {
                const uint8_t* row = im->pixels + y * im->stride;
                for (uint32_t x = 0; x < im->width && im->opaque; x++)
                    im->opaque = row[x] < count;
            }
            break;
        }
        default:
            im->opaque = false;         /* A mask - only ICON draws it */
            break;
    }
    return true;
}

/* The header of a file of `size` bytes: the magic, the version, the size;
 * the compression name terminated and zero-padded */
static bool check_header(const uint8_t* h, uint32_t size)
{
    if (h[0] != DMVI_MAGIC_0 || h[1] != DMVI_MAGIC_1 || h[2] != DMVI_MAGIC_2 || h[3] != DMVI_MAGIC_3 ||
        rd16(h + 4) != DMVI_VERSION_MAJOR || rd32(h + 8) != size || rd32(h + 52) > 0x7FFFFFFFu - HEADER)
        return false;
    bool end = false;
    for (uint32_t i = 0; i < DMVI_COMPRESSION_SIZE; i++)
    {
        if (end && h[40 + i] != 0)
            return false;
        end = end || h[40 + i] == 0;
    }
    return end;
}

/* The whole image in memory - the header, then what follows it, unpacked
 * when it is compressed. Returns the image's size, 0 when it fails. */
static uint32_t read_image(image_t* im, const uint8_t* h, uint32_t size, void* file)
{
    const char* compression = (const char*)h + 40;
    uint32_t unpacked = rd32(h + 52), packed = size - HEADER;
    if ((im->data = Dmod_Malloc(HEADER + unpacked + 1U)) == NULL)
        return 0;
    memcpy(im->data, h, HEADER);
    if (compression[0] == '\0')
        return (unpacked == packed && Dmod_FileRead(im->data + HEADER, 1, packed, file) == packed) ? HEADER + unpacked : 0;

    if (!Dmod_Compression_IsSupported(compression))
    {
        DMOD_LOG_WARN("libdmview: no %s decompression for %s\n", compression, im->path);
        return 0;
    }
    uint8_t* blob = Dmod_Malloc(packed + 1U);
    bool ok = blob != NULL && packed != 0 && Dmod_FileRead(blob, 1, packed, file) == packed &&
              Dmod_Compression_Unpack(compression, im->data + HEADER, unpacked, blob, packed) == unpacked;
    if (blob != NULL)
        Dmod_Free(blob);
    return ok ? HEADER + unpacked : 0;
}

static image_t* load(const char* path)
{
    size_t size = 0;
    uint8_t header[sizeof(dmvi_header_t)];
    void* file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
        return NULL;
    image_t* im = NULL;
    if (Dmod_FileSizeToSizeT(Dmod_FileSize(file), &size) && size <= 0x7FFFFFFFu && size >= HEADER &&
        (im = Dmod_Malloc(sizeof(*im))) != NULL)
    {
        memset(im, 0, sizeof(*im));
        im->path = Dmod_StrDup(path);
        im->state = IMAGE_READY;
        uint32_t image_size = 0;
        if (im->path == NULL || Dmod_FileRead(header, 1, HEADER, file) != HEADER ||
            !check_header(header, (uint32_t)size) || (image_size = read_image(im, header, (uint32_t)size, file)) == 0 ||
            !check(im, image_size) || !prepare(im))
        {
            DMOD_LOG_WARN("libdmview: %s is not a usable image\n", path);
            free_image(im);
            im = NULL;
        }
    }
    else if (size < HEADER)
        DMOD_LOG_WARN("libdmview: %s is not a usable image\n", path);
    Dmod_FileClose(file);
    return im;
}

static image_t* acquire(const char* path)
{
    if (g_images_lock == NULL)
        return NULL;
    dmosi_mutex_lock(g_images_lock);
    image_t* im = g_images;
    while (im != NULL && (im->stale || strcmp(im->path, path) != 0))
        im = im->next;
    if (im == NULL && (im = load(path)) != NULL)
    {
        im->next = g_images;
        g_images = im;
    }
    if (im != NULL)
        im->refs++;
    dmosi_mutex_unlock(g_images_lock);
    return im;
}

/* One user less (under the lock): the last one frees it */
static void release_locked(image_t* im)
{
    if (--im->refs != 0)
        return;
    for (image_t** p = &g_images; *p != NULL; p = &(*p)->next)
    {
        if (*p == im)
        {
            *p = im->next;
            break;
        }
    }
    free_image(im);
}

static void release(image_t* im)
{
    if (im == NULL || g_images_lock == NULL)
        return;
    dmosi_mutex_lock(g_images_lock);
    release_locked(im);
    dmosi_mutex_unlock(g_images_lock);
}

/* ---- The loader thread ---- */

/* The next image to read (under the lock), held by the loader while it reads it; NULL: none */
static image_t* next_to_load(void)
{
    for (image_t* im = g_images; im != NULL; im = im->next)
    {
        if (im->state == IMAGE_LOADING && !im->reading && im->refs > 0)
        {
            im->refs++;
            im->reading = true;
            return im;
        }
    }
    return NULL;
}

static void loader_thread(void* arg)
{
    (void)arg;
    while (!g_loader_quit)
    {
        dmosi_mutex_lock(g_images_lock);
        image_t* im = next_to_load();
        char* path = (im != NULL) ? Dmod_StrDup(im->path) : NULL;
        dmosi_mutex_unlock(g_images_lock);
        if (im == NULL)
        {
            (void)dmosi_semaphore_wait(g_loader_wake, 1, LOADER_WAIT_MS);
            continue;
        }
        image_t* read = (path != NULL) ? load(path) : NULL;
        if (read == NULL)
            DMOD_LOG_WARN("libdmview: cannot show the image %s\n", im->path);
        dmosi_mutex_lock(g_images_lock);
        if (read != NULL)
        {
            /* What was read is the image's now; its shell goes */
            im->opaque = read->opaque;
            im->format = read->format;
            im->width = read->width;
            im->height = read->height;
            im->stride = read->stride;
            im->alpha_stride = read->alpha_stride;
            im->data = read->data;
            im->pixels = read->pixels;
            im->alpha = read->alpha;
            im->palette = read->palette;
            read->data = NULL;
            read->palette = NULL;
        }
        im->reading = false;
        im->state = (read != NULL) ? IMAGE_READY : IMAGE_FAILED;
        g_loaded++;
        release_locked(im);                 /* (the loader's hold: no view wants it any more - freed) */
        dmosi_mutex_unlock(g_images_lock);
        if (read != NULL)
            free_image(read);
        if (path != NULL)
            Dmod_Free(path);
    }
}

/*
 * The image of a path for a slot whose source changed: the one known, else
 * a new one the loader reads (IMAGE_LOADING meanwhile)
 */
static image_t* acquire_later(const char* path)
{
    if (g_images_lock == NULL)
        return NULL;
    dmosi_mutex_lock(g_images_lock);
    image_t* im = g_images;
    while (im != NULL && (im->stale || strcmp(im->path, path) != 0))
        im = im->next;
    if (im == NULL && (im = Dmod_Malloc(sizeof(*im))) != NULL)
    {
        memset(im, 0, sizeof(*im));
        im->state = IMAGE_LOADING;
        if ((im->path = Dmod_StrDup(path)) == NULL)
        {
            Dmod_Free(im);
            im = NULL;
        }
        else
        {
            im->next = g_images;
            g_images = im;
        }
    }
    if (im != NULL)
        im->refs++;
    bool start = im != NULL && im->state == IMAGE_LOADING && g_loader == NULL;
    if (start && g_loader_wake == NULL)
        g_loader_wake = dmosi_semaphore_create(0, LOADER_WAKE_MAX);
    if (start && g_loader_wake != NULL)
    {
        /* Not a process's: a view service that stops does not take it with it */
        g_loader = dmosi_thread_create(loader_thread, NULL, dmosi_thread_get_priority(NULL), LOADER_STACK,
                                       "libdmview_images", NULL);
        if (g_loader == NULL)
            DMOD_LOG_ERROR("libdmview: no thread to read images\n");
    }
    dmosi_mutex_unlock(g_images_lock);
    if (im != NULL && im->state == IMAGE_LOADING && g_loader_wake != NULL)
        (void)dmosi_semaphore_post(g_loader_wake, 1);
    return im;
}

/* The image a slot draws now, NULL: none (yet) */
static const image_t* drawable(image_slot_t* sl)
{
    if (sl->image == NULL)
        return NULL;
    dmosi_mutex_lock(g_images_lock);
    uint8_t state = sl->image->state;
    dmosi_mutex_unlock(g_images_lock);
    sl->waiting = state == IMAGE_LOADING;
    return (state == IMAGE_READY) ? sl->image : NULL;
}

/* ---- Slots ---- */

static char* resolve(const struct libdmview* v, const char* source)
{
    if (source[0] == '/' || v->dir == NULL)
        return Dmod_StrDup(source);
    size_t ld = strlen(v->dir), ls = strlen(source);
    char* path = Dmod_Malloc(ld + ls + 2U);
    if (path == NULL)
        return NULL;
    memcpy(path, v->dir, ld);
    path[ld] = '/';
    memcpy(path + ld + 1U, source, ls + 1U);
    return path;
}

int images_slots(struct libdmview* v)
{
    uint32_t count = 0;
    for (uint32_t pc = 0; pc < v->code_size; pc += v->code[pc + 1U])
        count += (v->code[pc] == DMV_OP_IMAGE || v->code[pc] == DMV_OP_ICON) ? 1U : 0U;
    v->slot_count = count;
    if (count == 0)
        return 0;
    if ((v->slots = Dmod_Malloc(count * sizeof(image_slot_t))) == NULL)
        return -1;
    count = 0;
    for (uint32_t pc = 0; pc < v->code_size; pc += v->code[pc + 1U])
    {
        if (v->code[pc] != DMV_OP_IMAGE && v->code[pc] != DMV_OP_ICON)
            continue;
        image_slot_t* sl = &v->slots[count++];
        sl->pc = pc;
        sl->source = NULL;
        sl->image = NULL;
        sl->box = ROOT;
        sl->waiting = false;
    }
    return 0;
}

static void clear_slot(image_slot_t* sl)
{
    release(sl->image);
    sl->image = NULL;
    sl->waiting = false;
    if (sl->source != NULL)
        Dmod_Free(sl->source);
    sl->source = NULL;
}

void images_free_slots(struct libdmview* v)
{
    for (uint32_t i = 0; i < v->slot_count; i++)
        clear_slot(&v->slots[i]);
    if (v->slots != NULL)
        Dmod_Free(v->slots);
    v->slots = NULL;
    v->slot_count = 0;
}

const image_t* images_get(struct libdmview* v, uint32_t pc, const char* source)
{
    uint32_t lo = 0, hi = v->slot_count;
    while (lo < hi && v->slots[(lo + hi) / 2U].pc != pc)
    {
        if (v->slots[(lo + hi) / 2U].pc < pc)
            lo = (lo + hi) / 2U + 1U;
        else
            hi = (lo + hi) / 2U;
    }
    if (lo >= hi)
        return NULL;
    image_slot_t* sl = &v->slots[(lo + hi) / 2U];
    sl->box = v->frames[v->depth].box;
    if (sl->source != NULL && strcmp(sl->source, source) == 0)
        return drawable(sl);

    /* A new path: an empty one shows nothing, one that does not load is
     * logged once - until the path changes or RELOAD. The first one is read
     * now, a later one by the loader thread (see the top of the file) */
    bool first = sl->source == NULL;
    clear_slot(sl);
    if ((sl->source = Dmod_StrDup(source)) == NULL || source[0] == '\0')
        return NULL;
    char* path = resolve(v, source);
    if (path != NULL)
    {
        if (!first)
            sl->image = acquire_later(path);
        else if ((sl->image = acquire(path)) == NULL)
            DMOD_LOG_WARN("libdmview: cannot show the image %s\n", path);
        Dmod_Free(path);
    }
    return drawable(sl);
}

void images_poll(struct libdmview* v)
{
    uint32_t loaded = g_loaded;
    if (loaded == v->images_seen)
        return;
    v->images_seen = loaded;
    for (uint32_t i = 0; i < v->slot_count; i++)
    {
        image_slot_t* sl = &v->slots[i];
        if (!sl->waiting)
            continue;
        dmosi_mutex_lock(g_images_lock);
        bool done = sl->image == NULL || sl->image->state != IMAGE_LOADING;
        dmosi_mutex_unlock(g_images_lock);
        if (done)
        {
            sl->waiting = false;
            view_mark_dirty(v, sl->box);
        }
    }
}

void images_reload(struct libdmview* v, const char* source)
{
    char* path = resolve(v, source);
    if (path == NULL || g_images_lock == NULL)
    {
        if (path != NULL)
            Dmod_Free(path);
        return;
    }

    /* New users read the file again; the old image lives on with its users */
    dmosi_mutex_lock(g_images_lock);
    for (image_t* im = g_images; im != NULL; im = im->next)
    {
        if (strcmp(im->path, path) == 0)
            im->stale = true;
    }
    dmosi_mutex_unlock(g_images_lock);

    for (uint32_t i = 0; i < v->slot_count; i++)
    {
        image_slot_t* sl = &v->slots[i];
        if (sl->source == NULL)
            continue;
        char* shown = resolve(v, sl->source);
        if (shown != NULL && strcmp(shown, path) == 0)
        {
            clear_slot(sl);
            view_mark_dirty(v, sl->box);
        }
        if (shown != NULL)
            Dmod_Free(shown);
    }
    Dmod_Free(path);
}

/* ---- Drawing ---- */

/* Where the image lies in x, y, w, h by the alignment flags, and the part of
 * it that is drawn: inside that rectangle and the clip */
static bool place(const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h, const image_t* im, uint8_t align,
                  int32_t* ix, int32_t* iy, rect_t* part)
{
    int32_t iw = im->width, ih = im->height;
    switch (align & DMV_ALIGN_HMASK)
    {
        case DMV_ALIGN_CENTER: *ix = x + (w - iw) / 2; break;
        case DMV_ALIGN_RIGHT:  *ix = x + w - iw; break;
        default:               *ix = x; break;
    }
    switch (align & DMV_ALIGN_VMASK)
    {
        case DMV_ALIGN_MIDDLE: *iy = y + (h - ih) / 2; break;
        case DMV_ALIGN_BOTTOM: *iy = y + h - ih; break;
        default:               *iy = y; break;
    }
    rect_t area = { x, y, x + w, y + h }, pic = { *ix, *iy, *ix + iw, *iy + ih };
    *part = rect_and(rect_and(pic, &area), clip);
    return !rect_empty(part);
}

static inline uint32_t from_rgb565(uint32_t p)
{
    uint32_t r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

/* `n` pixels of row `sy` from column `sx`, as 0xAARRGGBB */
static void fetch_colors(const image_t* im, int32_t sx, int32_t sy, int32_t n, uint32_t* out)
{
    const uint8_t* row = im->pixels + (uint32_t)sy * im->stride;
    switch (im->format)
    {
        case DMVI_FORMAT_RGB565:
        {
            const uint16_t* p = (const uint16_t*)row + sx;
            for (int32_t i = 0; i < n; i++)
                out[i] = from_rgb565(p[i]);
            break;
        }
        case DMVI_FORMAT_ARGB8888:
            memcpy(out, (const uint32_t*)row + sx, (size_t)n * sizeof(uint32_t));
            break;
        case DMVI_FORMAT_RGB565A8:
        {
            const uint16_t* p = (const uint16_t*)row + sx;
            const uint8_t* a = im->alpha + (uint32_t)sy * im->alpha_stride + sx;
            for (int32_t i = 0; i < n; i++)
                out[i] = (from_rgb565(p[i]) & 0x00FFFFFFu) | ((uint32_t)a[i] << 24);
            break;
        }
        default:            /* I8 - masks are never drawn as colors */
            for (int32_t i = 0; i < n; i++)
                out[i] = im->palette[row[sx + i]];
            break;
    }
}

/* `n` pixels' coverage (alpha) of row `sy` from column `sx` */
static void fetch_coverage(const image_t* im, int32_t sx, int32_t sy, int32_t n, uint8_t* out)
{
    const uint8_t* row = im->pixels + (uint32_t)sy * im->stride;
    switch (im->format)
    {
        case DMVI_FORMAT_A8:
            memcpy(out, row + sx, (size_t)n);
            break;
        case DMVI_FORMAT_A4:
            for (int32_t i = 0; i < n; i++)
            {
                uint32_t b = row[(uint32_t)(sx + i) / 2U];
                out[i] = (uint8_t)((((sx + i) & 1) ? (b >> 4) : (b & 0x0Fu)) * 17U);
            }
            break;
        case DMVI_FORMAT_RGB565:
            memset(out, 0xFF, (size_t)n);
            break;
        case DMVI_FORMAT_RGB565A8:
            memcpy(out, im->alpha + (uint32_t)sy * im->alpha_stride + sx, (size_t)n);
            break;
        case DMVI_FORMAT_ARGB8888:
        {
            const uint8_t* p = row + (uint32_t)sx * 4U + 3U;
            for (int32_t i = 0; i < n; i++)
                out[i] = p[i * 4];
            break;
        }
        default:            /* I8 */
            for (int32_t i = 0; i < n; i++)
                out[i] = (uint8_t)(im->palette[row[sx + i]] >> 24);
            break;
    }
}

/* `n` colors into row `row` of the surface from column `x`, faded by `alpha` */
static void put_colors(const libdmview_surface_t* s, uint8_t* row, int32_t x, int32_t n, const uint32_t* c,
                       uint32_t alpha)
{
    if (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565)
    {
        uint16_t* p = (uint16_t*)row + x;
        for (int32_t i = 0; i < n; i++)
        {
            uint32_t a = c[i] >> 24;
            if (alpha < 255u)
                a = (a * alpha + 127u) / 255u;
            if (a == 255u)
                p[i] = to_rgb565(c[i]);
            else if (a != 0)
                p[i] = blend565(p[i], (c[i] & 0x00FFFFFFu) | (a << 24));
        }
        return;
    }
    uint32_t* p = (uint32_t*)row + x;
    for (int32_t i = 0; i < n; i++)
    {
        uint32_t a = c[i] >> 24;
        if (alpha < 255u)
            a = (a * alpha + 127u) / 255u;
        if (a == 255u)
            p[i] = c[i] | 0xFF000000u;
        else if (a != 0)
            p[i] = blend8888(p[i], (c[i] & 0x00FFFFFFu) | (a << 24));
    }
}

void draw_image(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
                const image_t* im, uint32_t alpha, uint8_t align)
{
    int32_t ix, iy;
    rect_t part;
    if (im->format == DMVI_FORMAT_A8 || im->format == DMVI_FORMAT_A4 || alpha == 0 ||
        !place(clip, x, y, w, h, im, align, &ix, &iy, &part))
        return;

    /* An opaque image in the surface's own format is copied row by row */
    uint32_t bpp = (s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565) ? 2U : 4U;
    bool copy = im->opaque && alpha == 255u &&
                ((im->format == DMVI_FORMAT_RGB565 && s->format == DMDRVI_GFX_PIXEL_FORMAT_RGB565) ||
                 (im->format == DMVI_FORMAT_ARGB8888 && s->format == DMDRVI_GFX_PIXEL_FORMAT_ARGB8888));
    int32_t n = part.x1 - part.x0, sx = part.x0 - ix;
    for (int32_t py = part.y0; py < part.y1; py++)
    {
        uint8_t* row = (uint8_t*)s->pixels + (uint32_t)py * s->stride;
        int32_t sy = py - iy;
        if (copy)
        {
            memcpy(row + (uint32_t)part.x0 * bpp, im->pixels + (uint32_t)sy * im->stride + (uint32_t)sx * bpp,
                   (size_t)n * bpp);
            continue;
        }
        for (int32_t done = 0; done < n; done += CHUNK)
        {
            uint32_t colors[CHUNK];
            int32_t k = (n - done < CHUNK) ? n - done : CHUNK;
            fetch_colors(im, sx + done, sy, k, colors);
            put_colors(s, row, part.x0 + done, k, colors, alpha);
        }
    }
}

void draw_icon(const libdmview_surface_t* s, const rect_t* clip, int32_t x, int32_t y, int32_t w, int32_t h,
               const image_t* im, const paint_t* paint, uint8_t align)
{
    int32_t ix, iy;
    rect_t part;
    if (!place(clip, x, y, w, h, im, align, &ix, &iy, &part))
        return;
    int32_t n = part.x1 - part.x0, sx = part.x0 - ix;
    for (int32_t py = part.y0; py < part.y1; py++)
    {
        for (int32_t done = 0; done < n; done += CHUNK)
        {
            uint8_t cover[CHUNK];
            int32_t k = (n - done < CHUNK) ? n - done : CHUNK;
            fetch_coverage(im, sx + done, py - iy, k, cover);
            for (int32_t i = 0; i < k; i++)
            {
                if (cover[i] != 0)
                    draw_cover(s, paint, part.x0 + done + i, py, cover[i]);
            }
        }
    }
}
