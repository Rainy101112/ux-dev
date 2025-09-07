/*
 *
 *      video_display.c
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

/* Clear only the display area with background color */
void video_clear_display_area(void)
{
    uint32_t end_x = display_offset_x + display_width;
    uint32_t end_y = display_offset_y + display_height;

    // Ensure we don't go beyond screen bounds
    if (end_x > width) end_x = width;
    if (end_y > height) end_y = height;

    for (uint32_t y = display_offset_y; y < end_y; y++) {
        for (uint32_t x = display_offset_x; x < end_x; x++) { buffer[y * stride + x] = back_color; }
    }

    // Reset cursor to start of display area
    cx = 0;
    cy = 0;
}

/* Draw a pixel at the specified coordinates relative to display area */
void video_draw_pixel_display(uint32_t dx, uint32_t dy, uint32_t color)
{
    uint32_t screen_x = display_offset_x + dx;
    uint32_t screen_y = display_offset_y + dy;

    // Only draw if within display area and screen bounds
    if (screen_x < width && screen_y < height && dx < display_width && dy < display_height) { buffer[screen_y * stride + screen_x] = color; }
}

/* Get a pixel at the specified coordinates relative to display area */
uint32_t video_get_pixel_display(uint32_t dx, uint32_t dy)
{
    uint32_t screen_x = display_offset_x + dx;
    uint32_t screen_y = display_offset_y + dy;

    // Only get if within display area and screen bounds, otherwise return background color
    if (screen_x < width && screen_y < height && dx < display_width && dy < display_height) { return buffer[screen_y * stride + screen_x]; }
    return back_color;
}

/* Iterate over a area relative to display area and run a callback function */
void video_invoke_display_area(position_t p0, position_t p1, void (*callback)(position_t p))
{
    // Convert to screen coordinates
    position_t screen_p0 = {display_offset_x + p0.x, display_offset_y + p0.y};
    position_t screen_p1 = {display_offset_x + p1.x, display_offset_y + p1.y};

    // Clamp to screen bounds
    if (screen_p0.x >= width || screen_p0.y >= height) return;
    if (screen_p1.x >= width) screen_p1.x = width - 1;
    if (screen_p1.y >= height) screen_p1.y = height - 1;

    video_invoke_area(screen_p0, screen_p1, callback);
}

/* Draw a rectangle relative to display area */
void video_draw_rect_display(position_t p0, position_t p1, uint32_t color)
{
    // Convert to screen coordinates
    position_t screen_p0 = {display_offset_x + p0.x, display_offset_y + p0.y};
    position_t screen_p1 = {display_offset_x + p1.x, display_offset_y + p1.y};

    video_draw_rect(screen_p0, screen_p1, color);
}

/* Set display size for character rendering */
void video_display_size_set(uint32_t width, uint32_t height)
{
    display_width    = width;
    display_height   = height;
    display_c_width  = width / 9;
    display_c_height = height / 16;

    // Reset cursor position when display size changes
    cx = 0;
    cy = 0;
}

/* Set display position on screen */
void video_display_position_set(uint32_t x, uint32_t y)
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
}

/* Get display position */
void video_display_position_get(uint32_t *x, uint32_t *y)
{
    *x = display_offset_x;
    *y = display_offset_y;
}

/* Move cursor to specified position within display area */
void video_move_to(uint32_t c_x, uint32_t c_y)
{
    // Clamp to display area
    if (c_x >= display_c_width) c_x = display_c_width - 1;
    if (c_y >= display_c_height) c_y = display_c_height - 1;

    cx = c_x;
    cy = c_y;
}
