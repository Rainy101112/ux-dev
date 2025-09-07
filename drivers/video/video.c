/*
 *
 *      video.c
 *      Basic Video
 *
 *      2024/9/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "video.h"
#include "common.h"
#include "cpuid.h"
#include "gfx_proc.h"
#include "limine.h"
#include "stddef.h"
#include "stdint.h"
#include "uinxed.h"
#include "string.h"

#define CPU_FEATURE_SSE 1

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

extern uint8_t ascii_font[]; // Fonts

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

static uint32_t glyph_cache_memory[MAX_CACHE_SIZE * 9 * 16] = {0};

static dirty_region_t dirty_region = {0};
static glyph_cache_t  glyph_cache[MAX_CACHE_SIZE] = {0};
static uint32_t       cache_timestamp = 0;
static bool           double_buffering_enabled = false;

#define MAX_BUFFER_SIZE (3840 * 2160 * 2)
static uint32_t back_buffer[MAX_BUFFER_SIZE / sizeof(uint32_t)] = {0};
static uint32_t back_buffer_stride = 0;

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = info.width / 9;
    info.c_height   = info.height / 16;
    info.cx         = cx;
    info.cy         = cy;
    info.fore_color = fore_color;
    info.back_color = back_color;

    info.bpp              = framebuffer->bpp;
    info.memory_model     = framebuffer->memory_model;
    info.red_mask_size    = framebuffer->red_mask_size;
    info.red_mask_shift   = framebuffer->red_mask_shift;
    info.green_mask_size  = framebuffer->green_mask_size;
    info.green_mask_shift = framebuffer->green_mask_shift;
    info.blue_mask_size   = framebuffer->blue_mask_size;
    info.blue_mask_shift  = framebuffer->blue_mask_shift;
    info.edid_size        = framebuffer->edid_size;
    info.edid             = framebuffer->edid;

    return info;
}

/* Get the frame buffer */
struct limine_framebuffer *get_framebuffer(void)
{
    return framebuffer_request.response->framebuffers[0];
}

/* Initialize Video */
void video_init(void)
{
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) krn_halt();
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    buffer                                 = framebuffer->address;
    width                                  = framebuffer->width;
    height                                 = framebuffer->height;
    stride                                 = framebuffer->pitch / (framebuffer->bpp / 8);

    x = cx = y = cy = 0;
    c_width         = width / 9;
    c_height        = height / 16;

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    
    /* Initialize the dirty rectangle technology */
    dirty_region.dirty = false;
    
    /* Initialize double buffering */
#if DOUBLE_BUFFERING
    uint64_t required_size = stride * height * sizeof(uint32_t);
    if (required_size <= sizeof(back_buffer)) {
        back_buffer_stride = stride;
        double_buffering_enabled = true;
        /* Clean back buffer */
        for (uint32_t i = 0; i < stride * height; i++) {
            back_buffer[i] = back_color;
        }
    }
#endif

    video_init_cache();

    video_clear();
}

/* Mark dirty region */
void video_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!dirty_region.dirty) {
        /* First dirty region */
        dirty_region.x1 = x;
        dirty_region.y1 = y;
        dirty_region.x2 = x + w;
        dirty_region.y2 = y + h;
        dirty_region.dirty = true;
    } else {
        /* Merge to current dirty region */
        dirty_region.x1 = MIN(dirty_region.x1, x);
        dirty_region.y1 = MIN(dirty_region.y1, y);
        dirty_region.x2 = MAX(dirty_region.x2, x + w);
        dirty_region.y2 = MAX(dirty_region.y2, y + h);
    }
    
    /* Limit */
    dirty_region.x2 = MIN(dirty_region.x2, width);
    dirty_region.y2 = MIN(dirty_region.y2, height);
}

/* 初始化字体缓存 */
void video_init_cache(void)
{
    for (int i = 0; i < MAX_CACHE_SIZE; i++) {
        glyph_cache[i].valid = false;
        glyph_cache[i].bitmap = NULL;
        glyph_cache[i].timestamp = 0;
    }
}

/* Get glyph cache */
static glyph_cache_t *video_get_glyph_cache(char c, uint32_t color)
{
    uint32_t index = (uint8_t)c;
    
    if (glyph_cache[index].valid) {
        glyph_cache[index].timestamp = ++cache_timestamp;
        return &glyph_cache[index];
    }
    
    /* Use independent cache memory */
    glyph_cache[index].bitmap = &glyph_cache_memory[index * 9 * 16];
    
    /* Pre-render font */
    uint8_t *font = ascii_font + (size_t)c * 16;
    uint32_t *bitmap = glyph_cache[index].bitmap;
    
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 9; j++) {
            uint32_t pixel_color = (font[i] & (0x80 >> j)) ? color : back_color;
            bitmap[i * 9 + j] = pixel_color;
        }
    }
    
    glyph_cache[index].valid = true;
    glyph_cache[index].timestamp = ++cache_timestamp;
    return &glyph_cache[index];
}

/* Draw char with SSE optimization */
static void video_draw_char_sse(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    glyph_cache_t *cache = video_get_glyph_cache(c, color);
    if (cache == NULL || !cache->valid) return;
    
    uint32_t *target_buffer = double_buffering_enabled ? back_buffer : buffer;
    uint32_t target_stride = double_buffering_enabled ? back_buffer_stride : stride;
    
    uint32_t *src = cache->bitmap;
    uint32_t *dest = target_buffer + y * target_stride + x;
    
#if CPU_FEATURE_SSE
    if (cpu_support_sse()) {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j += 4) {
                int pixels_to_copy = MIN(4, 9 - j);
                if (pixels_to_copy == 4) {
                    __asm__ volatile(
                        "movdqu (%0), %%xmm0\n\t"
                        "movdqu %%xmm0, (%1)\n\t"
                        : 
                        : "r" (src + j), "r" (dest + j)
                        : "xmm0", "memory"
                    );
                } else {
                    /* Process the remaining pixies */
                    for (int k = 0; k < pixels_to_copy; k++) {
                        dest[j + k] = src[j + k];
                    }
                }
            }
            src += 9;
            dest += target_stride;
        }
    } else 
#endif
    {
        /* Copy normally */
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j++) {
                dest[j] = src[j];
            }
            src += 9;
            dest += target_stride;
        }
    }
    
    /* Mark dirty region */
    video_mark_dirty(x, y, 9, 16);
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    for (uint32_t i = 0; i < (stride * height); i++) target[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    for (uint32_t i = 0; i < (stride * height); i++) back_buffer[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

/* Scroll the screen to the specified coordinates */
void video_move_to(uint32_t c_x, uint32_t c_y)
{
    cx = c_x;
    cy = c_y;
}

/* Screen scroll */
void video_scroll(void)
{
    if ((uint32_t)cx >= c_width) {
        cx = 1;
        cy++;
    } else {
        cx++;
    }

    if ((uint32_t)cy >= c_height) {
        uint32_t scroll_height = height - 16;
        uint32_t scroll_size = stride * scroll_height * sizeof(uint32_t);
        
        if (double_buffering_enabled) {
            memmove(back_buffer, back_buffer + stride * 16, scroll_size);
        } else {
            memmove(buffer, buffer + stride * 16, scroll_size);
        }
        
        video_draw_rect((position_t){0, scroll_height}, 
                   (position_t){width, height}, back_color);
        
        cy = c_height - 1;
        
        /* Mark entire screen to dirty (Need to refresh) */
        video_mark_dirty(0, 0, width, height);
        video_refresh();
    }
}

void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    target[y * stride + x] = color;
}

uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    return target[y * stride + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++) {
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
    }
}

/* Refresh partial region */
void video_partial_refresh(void)
{
    if (!dirty_region.dirty) return;

    uint32_t width = dirty_region.x2 - dirty_region.x1;
    uint32_t height = dirty_region.y2 - dirty_region.y1;

    if (width == 0 || height == 0) return;

    for (uint32_t y = dirty_region.y1; y < dirty_region.y2; y++) {
        uint32_t *src = back_buffer + y * back_buffer_stride + dirty_region.x1;
        uint32_t *dest = buffer + y * stride + dirty_region.x1;
        
#if CPU_FEATURE_SSE
        if (cpu_support_sse()) {
            uint32_t remaining = width;
            while (remaining >= 4) {
                __asm__ volatile(
                    "movdqu (%0), %%xmm0\n\t"
                    "movdqu %%xmm0, (%1)\n\t"
                    : 
                    : "r" (src), "r" (dest)
                    : "xmm0", "memory"
                );
                src += 4;
                dest += 4;
                remaining -= 4;
            }
            /* Remaining pixles */
            while (remaining > 0) {
                *dest++ = *src++;
                remaining--;
            }
        } else 
#endif
        {
            /* Normal */
            for (uint32_t x = 0; x < width; x++) {
                dest[x] = src[x];
            }
        }
    }
    
    /* Clean dirty mark */
    dirty_region.dirty = false;
}

/* Refresh entire screen */
void video_refresh(void)
{
    if (!double_buffering_enabled) return;

    uint64_t total_pixels = stride * height;

#if CPU_FEATURE_SSE
    if (cpu_support_sse()) {
        uint32_t *src = back_buffer;
        uint32_t *dest = buffer;
        uint64_t remaining = total_pixels;
        
        while (remaining >= 4) {
            __asm__ volatile(
                "movdqu (%0), %%xmm0\n\t"
                "movdqu %%xmm0, (%1)\n\t"
                : 
                : "r" (src), "r" (dest)
                : "xmm0", "memory"
            );
            src += 4;
            dest += 4;
            remaining -= 4;
        }
        /* Remaining */
        while (remaining > 0) {
            *dest++ = *src++;
            remaining--;
        }
    } else 
#endif
    {
        /* Normal */
        for (uint64_t i = 0; i < total_pixels; i++) {
            buffer[i] = back_buffer[i];
        }
    }
}

/* Draw a matrix at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    
    uint32_t *target_buffer = double_buffering_enabled ? back_buffer : buffer;
    uint32_t target_stride = double_buffering_enabled ? back_buffer_stride : stride;
    
    for (uint32_t y = y0; y <= y1; y++) {
        uint32_t *line = target_buffer + y * target_stride + x0;
        size_t count = x1 - x0 + 1;
        
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(color) : "memory");
#else
        for (uint32_t x = x0; x <= x1; x++) {
            line[x] = color;
        }
#endif
    }
    
    video_mark_dirty(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

void video_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    video_draw_char_sse(c, x, y, color);
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, uint32_t color)
{
    uint32_t x;
    uint32_t y;
    if (c == '\n') {
        cy++;
        cx = 0;
        /* Try scroll (but it will do when next character is printed)
         * video_scroll();
         * cx = 0;
         */
        return;
    } else if (c == '\r') {
        cx = 0;
        return;
    } else if (c == '\t') {
        for (int i = 0; i < 8; i++) {
            /* Expand by video_put_char(' ', color) */
            video_scroll();
            x = (cx - 1) * 9;
            y = cy * 16;
            video_draw_char(c, x, y, color);
        }
        return;
    } else if (c == '\b' && cx > 0) { // Do not fill, just move cursor
        if ((long long)cx - 1 < 0) {
            cx = c_width - 1;
            if (cy != 0) cy -= 1;
            if (cy == 0) cx = 0, cy = 0;
        } else {
            cx--;
        }
        return;
    }
    video_scroll();

    // 在绘制字符后调用局部刷新
    video_draw_char(c, (cx - 1) * 9, cy * 16, color);
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
    for (; *str; ++str) video_put_char(*str, fore_color);
    video_partial_refresh();
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_put_char(*str, color);
    video_partial_refresh();
}
