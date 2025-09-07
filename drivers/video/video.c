/*
 *
 *      video.c
 *      Advanced video driver with heap support
 *      Supports dirty rectangles, double buffering, and character buffering
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
#include "alloc.h"

extern uint8_t ascii_font[]; // Fonts

// 全局变量
uint64_t  width;  // Screen width
uint64_t  height; // Screen height
uint64_t  stride; // Frame buffer line spacing
uint32_t *buffer; // Primary frame buffer

// 双缓冲相关
uint32_t *back_buffer;    // 后台缓冲区
uint8_t      double_buffering_enabled = 0;

// 显示区域控制
uint64_t display_width;
uint64_t display_height;
uint32_t display_c_width, display_c_height;
uint32_t display_offset_x, display_offset_y;

// 光标位置
uint32_t x, y;              // The current absolute cursor position
uint32_t cx, cy;            // The character position of the current cursor
uint32_t c_width, c_height; // Screen character width and height

// 颜色
uint32_t fore_color; // Foreground color
uint32_t back_color; // Background color

// 字符缓冲区（用于加速文本渲染）
typedef struct {
    char     character;
    uint32_t color;
    uint32_t x;
    uint32_t y;
} char_buffer_entry_t;

char_buffer_entry_t *char_buffer     = NULL;
uint32_t            char_buffer_size = 0;
uint32_t            char_buffer_cap  = 0;
uint8_t                char_buffering_enabled = 0;

// 脏矩形区域
typedef struct {
    uint32_t x1, y1, x2, y2;
} dirty_rect_t;

dirty_rect_t *dirty_rects     = NULL;
uint32_t      dirty_rects_size = 0;
uint32_t      dirty_rects_cap  = 0;
uint8_t          dirty_rects_enabled = 0;

/* Get video information */
video_info_t video_get_info(void)
{
    video_info_t               info;
    struct limine_framebuffer *framebuffer = get_framebuffer();

    info.framebuffer = framebuffer->address;

    info.width      = framebuffer->width;
    info.height     = framebuffer->height;
    info.stride     = framebuffer->pitch / (framebuffer->bpp / 8);
    info.c_width    = display_c_width;
    info.c_height   = display_c_height;
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

/* Initialize Video with heap support */
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

    video_display_size_set(width, height);
    video_display_position_set(0, 0);

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    
    // 初始化高级功能
    video_init_advanced_features();
    
    video_clear();
}

/* Initialize advanced video features */
void video_init_advanced_features(void)
{
    // 分配后台缓冲区
    back_buffer = (uint32_t *)malloc(stride * height * sizeof(uint32_t));
    if (back_buffer) {
        double_buffering_enabled = 1;
        // 初始化后台缓冲区为背景色
        for (uint32_t i = 0; i < stride * height; i++) {
            back_buffer[i] = back_color;
        }
    }
    
    // 分配字符缓冲区
    char_buffer_cap = 1024; // 初始容量
    char_buffer = (char_buffer_entry_t *)malloc(char_buffer_cap * sizeof(char_buffer_entry_t));
    if (char_buffer) {
        char_buffering_enabled = 1;
        char_buffer_size = 0;
    }
    
    // 分配脏矩形缓冲区
    dirty_rects_cap = 32; // 初始容量
    dirty_rects = (dirty_rect_t *)malloc(dirty_rects_cap * sizeof(dirty_rect_t));
    if (dirty_rects) {
        dirty_rects_enabled = 1;
        dirty_rects_size = 0;
    }
}

/* Set display size for character rendering */
void video_display_size_set(uint32_t width, uint32_t height)
{
    display_width = width;
    display_height = height;
    display_c_width = width / 9;
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
    
    // 标记整个显示区域为脏
    video_mark_dirty_rect(display_offset_x, display_offset_y, 
                         display_offset_x + display_width, 
                         display_offset_y + display_height);
}

/* Get display position */
void video_display_position_get(uint32_t *x, uint32_t *y)
{
    *x = display_offset_x;
    *y = display_offset_y;
}

/* Clear screen */
void video_clear(void)
{
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});
    
    if (double_buffering_enabled) {
        for (uint32_t i = 0; i < stride * height; i++) {
            back_buffer[i] = back_color;
        }
    } else {
        for (uint32_t i = 0; i < stride * height; i++) {
            buffer[i] = back_color;
        }
    }
    
    // 清除字符缓冲区
    if (char_buffering_enabled) {
        char_buffer_size = 0;
    }
    
    // 标记整个屏幕为脏
    video_mark_dirty_rect(0, 0, width, height);
    
    x  = 2;
    y  = 0;
    cx = cy = 0;
}

/* Clear only the display area */
void video_clear_display_area(void)
{
    uint32_t end_x = display_offset_x + display_width;
    uint32_t end_y = display_offset_y + display_height;
    
    // Ensure we don't go beyond screen bounds
    if (end_x > width) end_x = width;
    if (end_y > height) end_y = height;
    
    if (double_buffering_enabled) {
        for (uint32_t y = display_offset_y; y < end_y; y++) {
            for (uint32_t x = display_offset_x; x < end_x; x++) {
                back_buffer[y * stride + x] = back_color;
            }
        }
    } else {
        for (uint32_t y = display_offset_y; y < end_y; y++) {
            for (uint32_t x = display_offset_x; x < end_x; x++) {
                buffer[y * stride + x] = back_color;
            }
        }
    }
    
    // 标记显示区域为脏
    video_mark_dirty_rect(display_offset_x, display_offset_y, end_x, end_y);
    
    // Reset cursor to start of display area
    cx = 0;
    cy = 0;
}

/* Screen scrolling operation with display area support */
void video_scroll(void)
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
        
        if (double_buffering_enabled) {
            // Scroll the display area up by one line (16 pixels) in back buffer
            for (uint32_t y = start_y; y < end_y; y++) {
                for (uint32_t x = start_x; x < end_x; x++) {
                    back_buffer[y * stride + x] = back_buffer[(y + 16) * stride + x];
                }
            }
            
            // Clear the bottom line of the display area in back buffer
            for (uint32_t y = end_y; y < end_y + 16; y++) {
                for (uint32_t x = start_x; x < end_x; x++) {
                    back_buffer[y * stride + x] = back_color;
                }
            }
        } else {
            // Scroll the display area up by one line (16 pixels)
            for (uint32_t y = start_y; y < end_y; y++) {
                for (uint32_t x = start_x; x < end_x; x++) {
                    buffer[y * stride + x] = buffer[(y + 16) * stride + x];
                }
            }
            
            // Clear the bottom line of the display area
            for (uint32_t y = end_y; y < end_y + 16; y++) {
                for (uint32_t x = start_x; x < end_x; x++) {
                    buffer[y * stride + x] = back_color;
                }
            }
        }
        
        // 标记显示区域为脏
        video_mark_dirty_rect(start_x, start_y, end_x, end_y + 16);
        
        cy = display_c_height - 1;
    }
}

/* Draw a pixel at the specified coordinates relative to display area */
void video_draw_pixel_display(uint32_t dx, uint32_t dy, uint32_t color)
{
    uint32_t screen_x = display_offset_x + dx;
    uint32_t screen_y = display_offset_y + dy;
    
    // Only draw if within display area and screen bounds
    if (screen_x < width && screen_y < height && 
        dx < display_width && dy < display_height) {
        
        if (double_buffering_enabled) {
            back_buffer[screen_y * stride + screen_x] = color;
        } else {
            buffer[screen_y * stride + screen_x] = color;
        }
        
        // 标记单个像素为脏（优化：可以合并到更大的脏矩形）
        video_mark_dirty_rect(screen_x, screen_y, screen_x + 1, screen_y + 1);
    }
}

/* Draw a character at the specified coordinates relative to display area */
void video_draw_char(const char c, uint32_t dx, uint32_t dy, uint32_t color)
{
    // Check if within display area
    if (dx + 8 >= display_width || dy + 15 >= display_height) return;

    uint32_t screen_x = display_offset_x + dx;
    uint32_t screen_y = display_offset_y + dy;

    uint8_t *font = ascii_font;
    font += (size_t)c * 16;

    // 如果启用了字符缓冲，将字符添加到缓冲区
    if (char_buffering_enabled) {
        if (char_buffer_size < char_buffer_cap) {
            char_buffer[char_buffer_size].character = c;
            char_buffer[char_buffer_size].color = color;
            char_buffer[char_buffer_size].x = dx;
            char_buffer[char_buffer_size].y = dy;
            char_buffer_size++;
        } else {
            // 缓冲区已满，立即渲染
            video_flush_char_buffer();
            // 重新添加到缓冲区
            char_buffer[0].character = c;
            char_buffer[0].color = color;
            char_buffer[0].x = dx;
            char_buffer[0].y = dy;
            char_buffer_size = 1;
        }
        return;
    }

    // 立即渲染字符
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 9; j++) {
            uint32_t pixel_x = screen_x + j;
            uint32_t pixel_y = screen_y + i;
            
            if (font[i] & (0x80 >> j)) {
                if (double_buffering_enabled) {
                    back_buffer[pixel_y * stride + pixel_x] = color;
                } else {
                    buffer[pixel_y * stride + pixel_x] = color;
                }
            } else {
                if (double_buffering_enabled) {
                    back_buffer[pixel_y * stride + pixel_x] = back_color;
                } else {
                    buffer[pixel_y * stride + pixel_x] = back_color;
                }
            }
        }
    }
    
    // 标记字符区域为脏
    video_mark_dirty_rect(screen_x, screen_y, screen_x + 9, screen_y + 16);
}

/* Print a character at the current cursor position */
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
        for (int i = 0; i < 4; i++) {
            video_put_char(' ', color);
        }
        return;
    } else if (c == '\b' && cx > 0) {
        cx--;
        // Clear the character at the backspace position
        uint32_t char_x = cx * 9;
        uint32_t char_y = cy * 16;
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j++) {
                video_draw_pixel_display(char_x + j, char_y + i, back_color);
            }
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

/* Mark a rectangle as dirty */
void video_mark_dirty_rect(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    if (!dirty_rects_enabled) return;
    
    // 简单的脏矩形管理：如果缓冲区已满，合并所有脏矩形为一个大的脏矩形
    if (dirty_rects_size >= dirty_rects_cap) {
        dirty_rects[0].x1 = 0;
        dirty_rects[0].y1 = 0;
        dirty_rects[0].x2 = width;
        dirty_rects[0].y2 = height;
        dirty_rects_size = 1;
        return;
    }
    
    // 添加新的脏矩形
    dirty_rects[dirty_rects_size].x1 = x1;
    dirty_rects[dirty_rects_size].y1 = y1;
    dirty_rects[dirty_rects_size].x2 = x2;
    dirty_rects[dirty_rects_size].y2 = y2;
    dirty_rects_size++;
}

/* Flush character buffer to screen */
void video_flush_char_buffer(void)
{
    if (!char_buffering_enabled || char_buffer_size == 0) return;
    
    for (uint32_t i = 0; i < char_buffer_size; i++) {
        char_buffer_entry_t *entry = &char_buffer[i];
        uint32_t screen_x = display_offset_x + entry->x;
        uint32_t screen_y = display_offset_y + entry->y;
        
        uint8_t *font = ascii_font;
        font += (size_t)entry->character * 16;
        
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 9; j++) {
                uint32_t pixel_x = screen_x + j;
                uint32_t pixel_y = screen_y + i;
                
                if (font[i] & (0x80 >> j)) {
                    if (double_buffering_enabled) {
                        back_buffer[pixel_y * stride + pixel_x] = entry->color;
                    } else {
                        buffer[pixel_y * stride + pixel_x] = entry->color;
                    }
                } else {
                    if (double_buffering_enabled) {
                        back_buffer[pixel_y * stride + pixel_x] = back_color;
                    } else {
                        buffer[pixel_y * stride + pixel_x] = back_color;
                    }
                }
            }
        }

        video_mark_dirty_rect(screen_x, screen_y, screen_x + 9, screen_y + 16);
    }
    
    char_buffer_size = 0;
}

/* Update screen (flush back buffer and dirty rectangles) */
void video_update(void)
{
    video_flush_char_buffer();
    
    if (double_buffering_enabled) {
        if (dirty_rects_enabled && dirty_rects_size > 0) {
            for (uint32_t i = 0; i < dirty_rects_size; i++) {
                dirty_rect_t *rect = &dirty_rects[i];
                for (uint32_t y = rect->y1; y < rect->y2; y++) {
                    for (uint32_t x = rect->x1; x < rect->x2; x++) {
                        buffer[y * stride + x] = back_buffer[y * stride + x];
                    }
                }
            }
            dirty_rects_size = 0;
        } else {
            for (uint32_t i = 0; i < stride * height; i++) {
                buffer[i] = back_buffer[i];
            }
        }
    }
}

/* Print a string */
void video_put_string(const char *str)
{
    for (; *str; ++str) video_put_char(*str, fore_color);
}

/* Print a string with color */
void video_put_string_color(const char *str, uint32_t color)
{
    for (; *str; ++str) video_put_char(*str, color);
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
