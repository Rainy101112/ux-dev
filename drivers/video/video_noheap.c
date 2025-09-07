/*
 *
 *      video_noheap.c
 *      Basic video driver with no heap
 *
 *      2024/9/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "common.h"
#include "cpuid.h"
#include "eis.h"
#include "gfx_proc.h"
#include "limine.h"
#include "stddef.h"
#include "stdint.h"
#include "uinxed.h"
#include "video.h"

extern uint8_t ascii_font[]; // Fonts

uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Video Memory (We think BPP is 32. If BPP is other value, you have to change it)

uint64_t display_width;
uint64_t display_height;
uint32_t display_c_width, display_c_height;
uint32_t display_offset_x, display_offset_y;

uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

int video_heaped_initialized = 0;

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = display_c_width;  // Use display character width instead of full screen
    info.c_height   = display_c_height; // Use display character height instead of full screen
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

    video_display_size_set(FRAMEBUFFER_NOHEAP_WIDTH, FRAMEBUFFER_NOHEAP_HEIGHT);
    video_display_position_set((width - FRAMEBUFFER_NOHEAP_WIDTH) / 2, (height - FRAMEBUFFER_NOHEAP_HEIGHT) / 2);

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    video_clear();
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

/* Clear screen with color */
void video_clear_color(uint32_t color)
{
    back_color = color;
    for (uint32_t i = 0; i < (stride * height); i++) buffer[i] = back_color;
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

void video_scroll(void)
{
    if (cx >= display_c_width) {
        cx = 0;
        cy++;
    }

    if (cy >= display_c_height) {
        uint32_t start_y = display_offset_y;
        uint32_t end_y   = display_offset_y + display_height - 16;
        uint32_t start_x = display_offset_x;
        uint32_t end_x   = display_offset_x + display_width;

        for (uint32_t y = start_y; y < end_y; y++) {
            for (uint32_t x = start_x; x < end_x; x++) { buffer[y * stride + x] = buffer[(y + 16) * stride + x]; }
        }

        for (uint32_t y = end_y; y < end_y + 16; y++) {
            for (uint32_t x = start_x; x < end_x; x++) { buffer[y * stride + x] = back_color; }
        }

        cy = display_c_height - 1;
    }
}

/* Draw a pixel at the specified coordinates on the screen */
void video_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    // Only draw if within screen bounds
    if (x < width && y < height) { buffer[y * stride + x] = color; }
}

/* Get a pixel at the specified coordinates on the screen */
uint32_t video_get_pixel(uint32_t x, uint32_t y)
{
    // Only get if within screen bounds, otherwise return background color
    if (x < width && y < height) { return buffer[y * stride + x]; }
    return back_color;
}

/* Iterate over a area on the screen and run a callback function in each iteration */
void video_invoke_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    position_t p;
    for (p.y = p0.y; p.y <= p1.y; p.y++) {
        for (p.x = p0.x; p.x <= p1.x; p.x++) callback(p);
    }
}

/* Draw a rectangle at the specified coordinates on the screen */
void video_draw_rect(position_t p0, position_t p1, uint32_t color)
{
    // Clamp to screen bounds
    if (p0.x >= width || p0.y >= height) return;
    if (p1.x >= width) p1.x = width - 1;
    if (p1.y >= height) p1.y = height - 1;

    uint32_t x0 = p0.x;
    uint32_t y0 = p0.y;
    uint32_t x1 = p1.x;
    uint32_t y1 = p1.y;
    for (y = y0; y <= y1; y++) {
        /* Draw horizontal line */
#if defined(__x86_64__) || defined(__i386__)
        uint32_t *line  = buffer + y * stride + x0;
        size_t    count = x1 - x0 + 1;
        __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(color) : "memory");
#else
        for (uint32_t x = x0; x <= x1; x++) video_draw_pixel(x, y, color);
#endif
    }
}

void video_draw_char(const char c, uint32_t dx, uint32_t dy, uint32_t color)
{
    if (dx + 8 >= display_width || dy + 15 >= display_height) return;

    uint32_t screen_x = display_offset_x + dx;
    uint32_t screen_y = display_offset_y + dy;

    uint8_t *font = ascii_font;
    font += (size_t)c * 16;

    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 9; j++) {
            if (font[i] & (0x80 >> j)) {
                video_draw_pixel(screen_x + j, screen_y + i, color);
            } else {
                video_draw_pixel(screen_x + j, screen_y + i, back_color);
            }
        }
    }
}

/* Print a character at the specified coordinates on the screen */
void video_put_char(const char c, uint32_t color)
{
    if (c == '\n') {
        cy++;
        cx = 0;
        video_scroll();
        return;
    } else if (c == '\r') {
        cx = 0;
        return;
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++) { // Reduced from 8 to 4 for better tab behavior
            video_put_char(' ', color);
        }
        return;
    } else if (c == '\b' && cx > 0) {
        cx--;
        // Clear the character at the backspace position
        uint32_t char_x = cx * 9;
        uint32_t char_y = cy * 16;
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j++) { video_draw_pixel_display(char_x + j, char_y + i, back_color); }
        }
        return;
    }

    // Only draw if within display area
    if (cx < display_c_width && cy < display_c_height) {
        uint32_t char_x = cx * 9;
        uint32_t char_y = cy * 16;
        video_draw_char(c, char_x, char_y, color);
        cx++;
    }

    video_scroll();
}

/* Print a string at the specified coordinates on the screen */
void video_put_string(const char *str)
{
    for (; *str; ++str) video_put_char(*str, fore_color);
}

/* Print a string with color at the specified coordinates on the screen */
void video_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_put_char(*str, color);
}
