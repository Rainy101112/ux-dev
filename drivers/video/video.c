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
#include "eis.h"
#include "alloc.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

extern uint8_t ascii_font[]; // Fonts

extern uint64_t  width;  // Screen width
extern uint64_t  height; // Screen height
extern uint64_t  stride; // Frame buffer line spacing
extern uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

extern uint64_t display_width;
extern uint64_t display_height;
extern uint32_t display_c_width, display_c_height;
extern uint32_t display_offset_x, display_offset_y;

extern uint32_t x, y;              // The current absolute cursor position
extern uint32_t cx, cy;            // The character position of the current cursor
extern uint32_t c_width, c_height; // Screen character width and height

extern uint32_t fore_color; // Foreground color
extern uint32_t back_color; // Background color

static uint32_t glyph_cache_memory[MAX_CACHE_SIZE * 9 * 16] = {0};

static dirty_region_t dirty_region = {0};
static glyph_cache_t  glyph_cache[MAX_CACHE_SIZE] = {0};
static uint32_t       cache_timestamp = 0;
static uint8_t           double_buffering_enabled = 0;

static uint32_t *back_buffer = NULL;
static uint32_t back_buffer_stride = 0;

/* Set display position on screen */
void video_heaped_display_position_set(uint32_t x, uint32_t y)
{
    // Ensure the display area stays within screen bounds
    if (x + display_width > width) {
        display_offset_x = (width > display_width) ? width - display_width : 0;
    } else {
        display_offset_x = x;
    }
    
    if (y + display_height > height) {
        display_offset_y = (height > display_height) ? height - display_height : 0;
    } else {
        display_offset_y = y;
    }
    
    // Reset cursor position when display position changes
    cx = 0;
    cy = 0;
    
    // Mark entire display area as dirty
    video_mark_dirty(display_offset_x, display_offset_y, display_width, display_height);
}

/* Get display position */
void video_heaped_display_position_get(uint32_t *x, uint32_t *y)
{
    *x = display_offset_x;
    *y = display_offset_y;
}

/* Clear only the display area */
void video_heaped_clear_display_area(void)
{
    uint32_t end_x = display_offset_x + display_width;
    uint32_t end_y = display_offset_y + display_height;
    
    // Ensure we don't go beyond screen bounds
    if (end_x > width) end_x = width;
    if (end_y > height) end_y = height;
    
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    
    for (uint32_t y = display_offset_y; y < end_y; y++) {
        for (uint32_t x = display_offset_x; x < end_x; x++) {
            target[y * stride + x] = back_color;
        }
    }
    
    // Mark display area as dirty
    video_mark_dirty(display_offset_x, display_offset_y, display_width, display_height);
    
    // Reset cursor to start of display area
    cx = 0;
    cy = 0;
}

/* Initialize Video */
void video_heaped_init(void)
{
	video_clear();
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) krn_halt();
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    buffer                                 = framebuffer->address;
    width                                  = framebuffer->width;
    height                                 = framebuffer->height;
    stride                                 = framebuffer->pitch / (framebuffer->bpp / 8);

    x = cx = y = cy = 0;
    c_width         = width / 9;
    c_height        = height / 16;

    // Default to full screen display area
    video_display_size_set(width, height);
    video_display_position_set(0, 0);

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    
    /* Initialize the dirty rectangle technology */
    dirty_region.dirty = 0;
    
    /* Initialize double buffering */
#if DOUBLE_BUFFERING
    uint64_t required_size = stride * height * sizeof(uint32_t);
    back_buffer = (uint32_t *)malloc(required_size);
    if (back_buffer) {
        back_buffer_stride = stride;
        double_buffering_enabled = 1;
        /* Clean back buffer */
        for (uint32_t i = 0; i < stride * height; i++) {
            back_buffer[i] = back_color;
        }
    }
#endif

    video_init_cache();

    video_heaped_clear();

	extern int video_heaped_initialized;
	video_heaped_initialized = 1;
}

/* Mark dirty region */
void video_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // Only mark areas within display region
    uint32_t display_end_x = display_offset_x + display_width;
    uint32_t display_end_y = display_offset_y + display_height;
    
    // Clamp to display area
    x = MAX(x, display_offset_x);
    y = MAX(y, display_offset_y);
    w = MIN(w, display_end_x - x);
    h = MIN(h, display_end_y - y);
    
    if (w == 0 || h == 0) return;
    
    if (!dirty_region.dirty) {
        /* First dirty region */
        dirty_region.x1 = x;
        dirty_region.y1 = y;
        dirty_region.x2 = x + w;
        dirty_region.y2 = y + h;
        dirty_region.dirty = 1;
    } else {
        /* Merge to current dirty region */
        dirty_region.x1 = MIN(dirty_region.x1, x);
        dirty_region.y1 = MIN(dirty_region.y1, y);
        dirty_region.x2 = MAX(dirty_region.x2, x + w);
        dirty_region.y2 = MAX(dirty_region.y2, y + h);
    }
    
    /* Limit to display area */
    dirty_region.x2 = MIN(dirty_region.x2, display_end_x);
    dirty_region.y2 = MIN(dirty_region.y2, display_end_y);
}

/* 初始化字体缓存 */
void video_init_cache(void)
{
    for (int i = 0; i < MAX_CACHE_SIZE; i++) {
        glyph_cache[i].valid = 0;
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
    
    glyph_cache[index].valid = 1;
    glyph_cache[index].timestamp = ++cache_timestamp;
    return &glyph_cache[index];
}

/* Draw char with SSE optimization */
static void video_draw_char_sse(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    // Check if within display area
    if (x < display_offset_x || y < display_offset_y ||
        x + 9 > display_offset_x + display_width || 
        y + 16 > display_offset_y + display_height) {
        return;
    }
    
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
void video_heaped_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    for (uint32_t i = 0; i < (stride * height); i++) target[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
    
    // Mark entire screen as dirty
    video_mark_dirty(0, 0, width, height);
}

/* Clear screen with color */
void video_heaped_clear_color(uint32_t color)
{
    back_color = color;
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    for (uint32_t i = 0; i < (stride * height); i++) target[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
    
    // Mark entire screen as dirty
    video_mark_dirty(0, 0, width, height);
}

/* Scroll the screen to the specified coordinates */
void video_heaped_move_to(uint32_t c_x, uint32_t c_y)
{
    // Clamp to display area
    if (c_x >= display_c_width) c_x = display_c_width - 1;
    if (c_y >= display_c_height) c_y = display_c_height - 1;
    
    cx = c_x;
    cy = c_y;
}

/* Screen scroll with display area support */
void video_heaped_scroll(void)
{
    if (cx >= display_c_width) {
        cx = 0;
        cy++;
    }

    if (cy >= display_c_height) {
        // Calculate the area to scroll (only the display area)
        uint32_t start_y = display_offset_y;
        uint32_t end_y = display_offset_y + display_height - 16;
        uint32_t start_x = display_offset_x;
        uint32_t end_x = display_offset_x + display_width;
        
        uint32_t *target_buffer = double_buffering_enabled ? back_buffer : buffer;
        uint32_t target_stride = double_buffering_enabled ? back_buffer_stride : stride;
        
        // Scroll the display area up by one line (16 pixels)
        for (uint32_t y = start_y; y < end_y; y++) {
            for (uint32_t x = start_x; x < end_x; x++) {
                target_buffer[y * target_stride + x] = target_buffer[(y + 16) * target_stride + x];
            }
        }
        
        // Clear the bottom line of the display area
        for (uint32_t y = end_y; y < end_y + 16; y++) {
            for (uint32_t x = start_x; x < end_x; x++) {
                target_buffer[y * target_stride + x] = back_color;
            }
        }
        
        cy = display_c_height - 1;
        
        /* Mark display area as dirty */
        video_mark_dirty(start_x, start_y, end_x - start_x, end_y + 16 - start_y);
    }
}

void video_heaped_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    // Only draw if within display area
    if (x < display_offset_x || y < display_offset_y ||
        x >= display_offset_x + display_width || 
        y >= display_offset_y + display_height) {
        return;
    }
    
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    target[y * stride + x] = color;
    
    // Mark single pixel as dirty
    video_mark_dirty(x, y, 1, 1);
}

uint32_t video_heaped_get_pixel(uint32_t x, uint32_t y)
{
    // Only get if within display area
    if (x < display_offset_x || y < display_offset_y ||
        x >= display_offset_x + display_width || 
        y >= display_offset_y + display_height) {
        return back_color;
    }
    
    uint32_t *target = double_buffering_enabled ? back_buffer : buffer;
    return target[y * stride + x];
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_heaped_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    // Clamp to display area
    p0.x = MAX(p0.x, display_offset_x);
    p0.y = MAX(p0.y, display_offset_y);
    p1.x = MIN(p1.x, display_offset_x + display_width - 1);
    p1.y = MIN(p1.y, display_offset_y + display_height - 1);
    
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++) {
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
    }
}

/* Refresh partial region */
void video_partial_refresh(void)
{
    if (!dirty_region.dirty || !double_buffering_enabled) return;

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
    dirty_region.dirty = 0;
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
void video_heaped_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    // Clamp to display area
    p0.x = MAX(p0.x, display_offset_x);
    p0.y = MAX(p0.y, display_offset_y);
    p1.x = MIN(p1.x, display_offset_x + display_width - 1);
    p1.y = MIN(p1.y, display_offset_y + display_height - 1);
    
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

void video_heaped_draw_char(const char c, uint32_t x, uint32_t y, uint32_t color)
{
    video_draw_char_sse(c, x, y, color);
}

/* Print a character at the specified coordinates on the screen */
void video_heaped_put_char(const char c, uint32_t color)
{
    if (c == '\n') {
        cy++;
        cx = 0;
        video_heaped_scroll();
        return;
    } else if (c == '\r') {
        cx = 0;
        return;
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++) {
            video_heaped_put_char(' ', color);
        }
        return;
    } else if (c == '\b' && cx > 0) {
        cx--;
        // Clear the character at the backspace position
        uint32_t char_x = display_offset_x + cx * 9;
        uint32_t char_y = display_offset_y + cy * 16;
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j++) {
                video_heaped_draw_pixel(char_x + j, char_y + i, back_color);
            }
        }
        return;
    }
    
    // Only draw if within display area
    if (cx < display_c_width && cy < display_c_height) {
        uint32_t char_x = display_offset_x + cx * 9;
        uint32_t char_y = display_offset_y + cy * 16;
        video_heaped_draw_char(c, char_x, char_y, color);
        cx++;
    }
    
    video_heaped_scroll();
}

/* Print a string at the specified coordinates on the screen */
void video_heaped_put_string(const char *str)
{
    for (; *str; ++str) video_heaped_put_char(*str, fore_color);
    video_partial_refresh();
}

/* Print a string with color at the specified coordinates on the screen */
void video_heaped_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_heaped_put_char(*str, color);
    video_partial_refresh();
}
