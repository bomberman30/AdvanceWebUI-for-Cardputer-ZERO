
/*
 * Simple Cardputer Zero + LVGL example
 *
 * Everything is in this one file.
 */

#include <cstdio>

// LVGL
#include <lvgl.h>

// Cardputer Zero DRM driver
#include "src/drivers/display/drm/lv_linux_drm.h"

// Linux input
#include "linux_input.h"

// ------------------------------------------------------------
// SETTINGS
// ------------------------------------------------------------

#define DRM_DEVICE "/dev/dri/card1"
#define DRM_CONNECTOR_ID 34

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 170

// ------------------------------------------------------------
// COLORS
// ------------------------------------------------------------

#define COLOR_BACKGROUND 0x101010
#define COLOR_WHITE      0xFFFFFF
#define COLOR_RED        0xFF0000
#define COLOR_GREEN      0x00FF00
#define COLOR_BLUE       0x0080FF
#define COLOR_YELLOW     0xFFFF00

// ------------------------------------------------------------
// MAIN
// ------------------------------------------------------------

int main()
{
    // --------------------------------------------------------
    // Initialize LVGL
    // --------------------------------------------------------

    lv_init();

    // --------------------------------------------------------
    // Initialize Cardputer Zero display
    // --------------------------------------------------------

    lv_display_t* display = lv_linux_drm_create();

    if (!display)
    {
        printf("ERROR: Failed to create DRM display\n");
        return 1;
    }

    if (lv_linux_drm_set_file(
            display,
            DRM_DEVICE,
            DRM_CONNECTOR_ID) != LV_RESULT_OK)
    {
        printf("ERROR: Failed to initialize DRM display\n");

        lv_display_delete(display);
        return 1;
    }

    // Keyboard / input
    platform::init_key_input(display);

    // --------------------------------------------------------
    // Background
    // --------------------------------------------------------

    lv_obj_t* screen = lv_screen_active();

    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(COLOR_BACKGROUND),
        LV_PART_MAIN
    );

    // --------------------------------------------------------
    // TITLE
    // --------------------------------------------------------

    lv_obj_t* title = lv_label_create(screen);

    lv_label_set_text(
        title,
        "Hello Cardputer Zero!"
    );

    lv_obj_set_style_text_color(
        title,
        lv_color_hex(COLOR_WHITE),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_font(
        title,
        LV_FONT_DEFAULT,
        LV_PART_MAIN
    );

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        10
    );

    // --------------------------------------------------------
    // RED RECTANGLE
    // --------------------------------------------------------

    lv_obj_t* red_box = lv_obj_create(screen);

    lv_obj_set_size(
        red_box,
        80,
        40
    );

    lv_obj_set_pos(
        red_box,
        20,
        60
    );

    lv_obj_set_style_bg_color(
        red_box,
        lv_color_hex(COLOR_RED),
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        red_box,
        0,
        LV_PART_MAIN
    );

    lv_obj_set_style_radius(
        red_box,
        5,
        LV_PART_MAIN
    );

    // --------------------------------------------------------
    // GREEN RECTANGLE
    // --------------------------------------------------------

    lv_obj_t* green_box = lv_obj_create(screen);

    lv_obj_set_size(
        green_box,
        80,
        40
    );

    lv_obj_set_pos(
        green_box,
        120,
        60
    );

    lv_obj_set_style_bg_color(
        green_box,
        lv_color_hex(COLOR_GREEN),
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        green_box,
        0,
        LV_PART_MAIN
    );

    lv_obj_set_style_radius(
        green_box,
        5,
        LV_PART_MAIN
    );

    // --------------------------------------------------------
    // BLUE CIRCLE
    // --------------------------------------------------------

    lv_obj_t* circle = lv_obj_create(screen);

    lv_obj_set_size(
        circle,
        50,
        50
    );

    lv_obj_set_pos(
        circle,
        235,
        55
    );

    lv_obj_set_style_bg_color(
        circle,
        lv_color_hex(COLOR_BLUE),
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        circle,
        0,
        LV_PART_MAIN
    );

    lv_obj_set_style_radius(
        circle,
        LV_RADIUS_CIRCLE,
        LV_PART_MAIN
    );

    // --------------------------------------------------------
    // YELLOW TEXT
    // --------------------------------------------------------

    lv_obj_t* text = lv_label_create(screen);

    lv_label_set_text(
        text,
        "LVGL is working!"
    );

    lv_obj_set_style_text_color(
        text,
        lv_color_hex(COLOR_YELLOW),
        LV_PART_MAIN
    );

    lv_obj_align(
        text,
        LV_ALIGN_BOTTOM_MID,
        0,
        -15
    );

    // --------------------------------------------------------
    // MAIN LOOP
    // --------------------------------------------------------

    while (true)
    {
        lv_timer_handler();

        lv_delay_ms(5);
    }

    return 0;
}

