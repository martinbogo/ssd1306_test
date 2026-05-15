#include "ssd1306.h"
#include <gui/gui.h>
#include <input/input.h>
#include <furi.h>
#include <furi_hal.h>
#include <stdlib.h>

// Display variant: yellow-bar split height (0 = full monochrome)
#define YELLOW_BAR_16  16
#define YELLOW_BAR_NONE 0

typedef enum {
    SceneMainMenu,
    ScenePatterns,
    SceneBrightness,
    SceneCommands,
    SceneScrolling,
    SceneInfo,
    SceneClock,
    ScenePlant,
} Scene;

typedef struct {
    Scene scene;
    int cursor;           // menu cursor position
    int scroll_offset;    // for scrollable menus

    SSD1306 oled;
    bool detected;

    // test pattern state
    int pattern_idx;

    // brightness
    uint8_t brightness;

    // command toggles
    bool cmd_invert;
    bool cmd_flip_h;
    bool cmd_flip_v;
    bool cmd_all_on;
    bool cmd_power;

    // scrolling
    int scroll_menu_idx;
    uint8_t scroll_speed;

    // variant
    uint8_t yellow_bar_height; // 0 or 16
    uint8_t detected_addr;     // 0x3C or 0x3D

    // clock
    uint8_t last_second;

    // plant
    uint32_t plant_growth;
    int32_t plant_water;

    bool running;
    FuriMutex* mutex;
} App;

static const char* weekday_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

// -- Pattern generators --

static void pattern_all_white(App* app) {
    ssd1306_fill(&app->oled, 0xFF);
    ssd1306_flush(&app->oled);
}

static void pattern_all_black(App* app) {
    ssd1306_clear(&app->oled);
    ssd1306_flush(&app->oled);
}

static void pattern_checkerboard(App* app) {
    for(int p = 0; p < SSD1306_PAGES; p++)
        for(int x = 0; x < SSD1306_WIDTH; x++)
            app->oled.buffer[p * SSD1306_WIDTH + x] =
                ((x + p) & 1) ? 0x55 : 0xAA;
    ssd1306_flush(&app->oled);
}

static void pattern_h_lines(App* app) {
    ssd1306_fill(&app->oled, 0x55); // alternating rows within each page
    ssd1306_flush(&app->oled);
}

static void pattern_v_lines(App* app) {
    for(int p = 0; p < SSD1306_PAGES; p++)
        for(int x = 0; x < SSD1306_WIDTH; x++)
            app->oled.buffer[p * SSD1306_WIDTH + x] = (x & 1) ? 0xFF : 0x00;
    ssd1306_flush(&app->oled);
}

static void pattern_border(App* app) {
    ssd1306_clear(&app->oled);
    ssd1306_rect(&app->oled, 0, 0, 128, 64);
    ssd1306_flush(&app->oled);
}

static void pattern_zone_test(App* app) {
    ssd1306_clear(&app->oled);
    uint8_t split = app->yellow_bar_height;
    if(split == 0) split = 16; // show 16px marker even on mono for reference
    // fill top zone
    ssd1306_fill_rect(&app->oled, 0, 0, 128, split);
    // label zones
    ssd1306_clear(&app->oled);
    ssd1306_fill_rect(&app->oled, 0, 0, 128, split);
    // draw boundary line (inverted in the filled area)
    for(int x = 0; x < 128; x += 2)
        ssd1306_pixel(&app->oled, x, split, true);
    // text labels using inverted pixels in top zone
    if(split >= 16) {
        // clear a text area in the filled zone
        for(int16_t y = 4; y < 11; y++)
            for(int16_t x = 20; x < 108; x++)
                ssd1306_pixel(&app->oled, x, y, false);
        // write in cleared area (these will be dark-on-light)
        ssd1306_string(&app->oled, 22, 4, "YELLOW ZONE");
    }
    ssd1306_string(&app->oled, 22, split + 8, "MAIN ZONE");
    ssd1306_string(&app->oled, 10, split + 24, "Boundary at row ");
    char num[4];
    snprintf(num, sizeof(num), "%d", split);
    ssd1306_string(&app->oled, 106, split + 24, num);
    ssd1306_flush(&app->oled);
}

static void pattern_page_fill(App* app) {
    // fill each page sequentially (useful for identifying page boundaries)
    for(int p = 0; p < SSD1306_PAGES; p++) {
        ssd1306_clear(&app->oled);
        memset(&app->oled.buffer[p * SSD1306_WIDTH], 0xFF, SSD1306_WIDTH);
        // label it
        char label[20];
        snprintf(label, sizeof(label), "Page %d (rows %d-%d)", p, p * 8, p * 8 + 7);
        ssd1306_string(&app->oled, 4, (p == 3 || p == 4) ? 0 : 28, label);
        ssd1306_flush(&app->oled);
        furi_delay_ms(800);
    }
    // show all pages labeled
    ssd1306_clear(&app->oled);
    for(int p = 0; p < SSD1306_PAGES; p++) {
        char label[4];
        snprintf(label, sizeof(label), "P%d", p);
        ssd1306_string(&app->oled, 56, p * 8, label);
    }
    ssd1306_flush(&app->oled);
}

static void pattern_gradient(App* app) {
    ssd1306_clear(&app->oled);
    // dithered gradient: more pixels lit toward the right
    for(int x = 0; x < 128; x++) {
        uint8_t density = (uint8_t)((x * 255) / 127);
        for(int y = 0; y < 64; y++) {
            // ordered dither 4x4
            static const uint8_t bayer4[4][4] = {
                {  0, 128,  32, 160},
                {192,  64, 224,  96},
                { 48, 176,  16, 144},
                {240, 112, 208,  80},
            };
            if(density > bayer4[y & 3][x & 3])
                ssd1306_pixel(&app->oled, x, y, true);
        }
    }
    ssd1306_flush(&app->oled);
}

static void pattern_shapes(App* app) {
    ssd1306_clear(&app->oled);
    ssd1306_rect(&app->oled, 2, 2, 40, 25);
    ssd1306_fill_rect(&app->oled, 6, 6, 12, 12);
    ssd1306_circle(&app->oled, 85, 15, 14);
    ssd1306_circle(&app->oled, 85, 15, 8);
    ssd1306_line(&app->oled, 0, 63, 127, 32);
    ssd1306_line(&app->oled, 0, 32, 127, 63);
    ssd1306_string(&app->oled, 2, 36, "SSD1306 Test App");
    ssd1306_string(&app->oled, 2, 46, "abcdefghijklmnopqrstu");
    ssd1306_string(&app->oled, 2, 56, "0123456789 !@#$%");
    ssd1306_flush(&app->oled);
}

typedef void (*PatternFunc)(App*);

static const char* pattern_names[] = {
    "All White",
    "All Black",
    "Checkerboard",
    "Horiz Lines",
    "Vert Lines",
    "Border",
    "Color Zones",
    "Page Fill",
    "Gradient",
    "Shapes & Text",
};

static PatternFunc pattern_funcs[] = {
    pattern_all_white,
    pattern_all_black,
    pattern_checkerboard,
    pattern_h_lines,
    pattern_v_lines,
    pattern_border,
    pattern_zone_test,
    pattern_page_fill,
    pattern_gradient,
    pattern_shapes,
};

#define PATTERN_COUNT ((int)(sizeof(pattern_names) / sizeof(pattern_names[0])))

// -- Flipper screen rendering (draw callback) --

static const char* main_menu_items[] = {
    "Clock + Status",
    "Test Patterns",
    "Brightness",
    "Display Commands",
    "Scrolling",
    "Display Info",
    "Botanical Obligation",
};
#define MAIN_MENU_COUNT 7

static const char* cmd_labels[] = {
    "Invert",
    "Flip Horizontal",
    "Flip Vertical",
    "Force All Pixels",
    "Display Power",
    "Variant: ",
};
#define CMD_COUNT 6

static const char* scroll_labels[] = {
    "Scroll Right",
    "Scroll Left",
    "Diagonal Up-Right",
    "Diagonal Up-Left",
    "Stop Scroll",
    "Speed: ",
};
#define SCROLL_COUNT 6

static void draw_menu(Canvas* canvas, const char** items, int count, int cursor, const char* title) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, title);
    canvas_draw_line(canvas, 0, 15, 128, 15);
    canvas_set_font(canvas, FontSecondary);
    int visible = 4;
    int offset = 0;
    if(cursor >= visible) offset = cursor - visible + 1;
    for(int i = 0; i < visible && (i + offset) < count; i++) {
        int idx = i + offset;
        int y = 27 + i * 12;
        if(idx == cursor) {
            canvas_draw_box(canvas, 0, y - 9, 128, 12);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 6, y, items[idx]);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void render_clock_oled(App* app) {
    SSD1306* d = &app->oled;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    uint8_t bat_pct = furi_hal_power_get_pct();
    bool charging = furi_hal_power_is_charging();

    ssd1306_clear(d);

    if(app->yellow_bar_height > 0) {
        // Yellow bar: filled background with inverted (dark) text/icons
        ssd1306_fill_rect(d, 0, 0, 128, 16);

        // battery icon (inverted: clear pixels in filled area)
        // draw outline by clearing, then re-adding body
        for(int16_t y = 3; y < 12; y++)
            for(int16_t x = 2; x < 18; x++)
                ssd1306_pixel(d, x, y, false);
        // re-draw battery outline in cleared area (dark lines on yellow)
        ssd1306_pixel(d, 2, 4, true); ssd1306_pixel(d, 2, 10, true);
        ssd1306_pixel(d, 14, 4, true); ssd1306_pixel(d, 14, 10, true);
        for(int16_t x = 3; x < 14; x++) {
            ssd1306_pixel(d, x, 3, true);
            ssd1306_pixel(d, x, 11, true);
        }
        for(int16_t y = 4; y < 11; y++) {
            ssd1306_pixel(d, 2, y, true);
            ssd1306_pixel(d, 14, y, true);
        }
        ssd1306_pixel(d, 15, 6, true);
        ssd1306_pixel(d, 15, 7, true);
        ssd1306_pixel(d, 15, 8, true);
        int fill = (bat_pct * 10) / 100;
        if(fill > 0)
            for(int16_t y = 5; y < 10; y++)
                for(int16_t x = 4; x < 4 + fill; x++)
                    ssd1306_pixel(d, x, y, true);

        // battery percentage text (dark on yellow = clear pixels for text bg)
        char pct_str[6];
        snprintf(pct_str, sizeof(pct_str), "%d%%", bat_pct);
        // clear area for text, then draw dark chars
        for(int16_t y = 4; y < 12; y++)
            for(int16_t x = 18; x < 42; x++)
                ssd1306_pixel(d, x, y, false);
        ssd1306_string(d, 19, 4, pct_str);
        // invert the text chars (make them dark-on-yellow)
        // Actually: text was drawn as lit pixels in a cleared area,
        // so on yellow bar they appear as light text on dark cutout. Perfect.

        if(charging) {
            for(int16_t y = 4; y < 12; y++)
                for(int16_t x = 40; x < 48; x++)
                    ssd1306_pixel(d, x, y, false);
            ssd1306_char(d, 41, 4, '+');
        }

        // day of week (centered)
        const char* dow = (dt.weekday >= 1 && dt.weekday <= 7)
            ? weekday_names[dt.weekday - 1] : "???";
        int16_t dow_x = 55;
        for(int16_t y = 4; y < 12; y++)
            for(int16_t x = dow_x - 2; x < dow_x + 20; x++)
                ssd1306_pixel(d, x, y, false);
        ssd1306_string(d, dow_x, 4, dow);

        // date (right side)
        char date_str[12];
        snprintf(date_str, sizeof(date_str), "%02d/%02d/%02d",
            dt.month, dt.day, dt.year % 100);
        int16_t date_x = 80;
        for(int16_t y = 4; y < 12; y++)
            for(int16_t x = date_x - 2; x < 128; x++)
                ssd1306_pixel(d, x, y, false);
        ssd1306_string(d, date_x, 4, date_str);

    } else {
        // Mono display: thin status line at top with no fill
        ssd1306_battery_icon(d, 2, 2, bat_pct, charging);

        char pct_str[6];
        snprintf(pct_str, sizeof(pct_str), "%d%%", bat_pct);
        ssd1306_string(d, 20, 4, pct_str);

        const char* dow = (dt.weekday >= 1 && dt.weekday <= 7)
            ? weekday_names[dt.weekday - 1] : "???";
        ssd1306_string(d, 50, 4, dow);

        char date_str[12];
        snprintf(date_str, sizeof(date_str), "%02d/%02d/%02d",
            dt.month, dt.day, dt.year % 100);
        ssd1306_string(d, 80, 4, date_str);

        ssd1306_line(d, 0, 13, 127, 13);
    }

    // Big clock: HH:MM in the main area
    // Digit cell: 20w x 28h, colon: 6w, gaps: 2px between elements
    // Total: 20+2+20+2+6+2+20+2+20 = 94px. Centered: x = (128-94)/2 = 17
    int16_t cx = 17;
    int16_t cy = 18; // below status bar

    uint8_t h10 = dt.hour / 10;
    uint8_t h1 = dt.hour % 10;
    uint8_t m10 = dt.minute / 10;
    uint8_t m1 = dt.minute % 10;

    ssd1306_big_digit(d, cx, cy, h10);
    ssd1306_big_digit(d, cx + 22, cy, h1);
    ssd1306_big_colon(d, cx + 43, cy, (dt.second & 1) == 0); // blink every second
    ssd1306_big_digit(d, cx + 50, cy, m10);
    ssd1306_big_digit(d, cx + 72, cy, m1);

    // Seconds in small font, bottom right of the clock
    char sec_str[8];
    snprintf(sec_str, sizeof(sec_str), ":%02d", dt.second);
    ssd1306_string(d, cx + 93, cy + 20, sec_str);

    // Device name at the bottom
    const char* name = furi_hal_version_get_name_ptr();
    if(name) {
        int16_t name_x = 64 - (int16_t)((strlen(name) * 6) / 2);
        ssd1306_string(d, name_x, 56, name);
    }

    ssd1306_flush(d);
    app->last_second = dt.second;
}

static void render_plant_oled(App* app) {
    SSD1306* d = &app->oled;
    ssd1306_clear(d);

    // ground
    ssd1306_line(d, 0, 60, 127, 60);

    // pot
    ssd1306_line(d, 54, 60, 50, 45);
    ssd1306_line(d, 74, 60, 78, 45);
    ssd1306_line(d, 50, 45, 78, 45);

    uint32_t max_growth = 100;
    uint32_t growth = app->plant_growth > max_growth ? max_growth : app->plant_growth;
    int x_center = 64;
    int y_base = 45;

    // stalk logic
    int height = (growth * 30) / max_growth;
    if (height > 0) {
        ssd1306_line(d, x_center, y_base, x_center, y_base - height);
    }

    // leaves (grow when taller)
    if (height > 10) {
        ssd1306_line(d, x_center, y_base - 10, x_center - 8, y_base - 15);
        ssd1306_line(d, x_center, y_base - 15, x_center + 8, y_base - 20);
    }

    // text/status 
    if (app->plant_water <= 0) {
        ssd1306_string(d, 2, 10, "Water me. Obviously.");
        // wilted leaves over stalk
        ssd1306_line(d, x_center, y_base - height, x_center + 10, y_base - height + 10);
    } else if (app->plant_water > 100) {
        ssd1306_string(d, 2, 10, "Drowning. Thanks.");
        // water level line above pot
        ssd1306_line(d, 48, 40, 80, 40);
        ssd1306_line(d, 48, 40, 48, 45);
        ssd1306_line(d, 80, 40, 80, 45);
    } else {
        ssd1306_string(d, 2, 10, "Growing. Slowly.");
        if (height > 0) {
            // flower head
            ssd1306_circle(d, x_center, y_base - height, 4);
        }
    }

    ssd1306_flush(d);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    App* app = (App*)ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(!app->detected) {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "No SSD1306 Found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Check wiring:");
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "SDA=C1(15) SCL=C0(16)");
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "VCC=3V3(9) GND(18)");
        furi_mutex_release(app->mutex);
        return;
    }

    switch(app->scene) {
    case SceneMainMenu:
        draw_menu(canvas, main_menu_items, MAIN_MENU_COUNT, app->cursor, "SSD1306 Test");
        break;

    case ScenePatterns:
        draw_menu(canvas, pattern_names, PATTERN_COUNT, app->cursor, "Test Patterns");
        break;

    case SceneBrightness: {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Brightness");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);
        char val[16];
        snprintf(val, sizeof(val), "%d / 255", app->brightness);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, val);
        // bar
        canvas_draw_frame(canvas, 8, 34, 112, 10);
        int fill_w = (app->brightness * 110) / 255;
        if(fill_w > 0) canvas_draw_box(canvas, 9, 35, fill_w, 8);
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, "< Left/Right to adjust >");
        break;
    }

    case SceneCommands: {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Commands");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);

        const bool toggles[] = {
            app->cmd_invert,
            app->cmd_flip_h,
            app->cmd_flip_v,
            app->cmd_all_on,
            app->cmd_power,
            false, // variant placeholder
        };

        int visible = 4;
        int offset = 0;
        if(app->cursor >= visible) offset = app->cursor - visible + 1;
        for(int i = 0; i < visible && (i + offset) < CMD_COUNT; i++) {
            int idx = i + offset;
            int y = 27 + i * 12;
            if(idx == app->cursor) {
                canvas_draw_box(canvas, 0, y - 9, 128, 12);
                canvas_set_color(canvas, ColorWhite);
            }
            char line[40];
            if(idx == 5) {
                snprintf(line, sizeof(line), "Variant: %s",
                    app->yellow_bar_height > 0 ? "Yellow-bar" : "Monochrome");
            } else {
                snprintf(line, sizeof(line), "%s: %s",
                    cmd_labels[idx], toggles[idx] ? "ON" : "OFF");
            }
            canvas_draw_str(canvas, 6, y, line);
            canvas_set_color(canvas, ColorBlack);
        }
        break;
    }

    case SceneScrolling: {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Scrolling");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);

        int visible = 4;
        int offset = 0;
        if(app->cursor >= visible) offset = app->cursor - visible + 1;
        for(int i = 0; i < visible && (i + offset) < SCROLL_COUNT; i++) {
            int idx = i + offset;
            int y = 27 + i * 12;
            if(idx == app->cursor) {
                canvas_draw_box(canvas, 0, y - 9, 128, 12);
                canvas_set_color(canvas, ColorWhite);
            }
            if(idx == 5) {
                char line[20];
                snprintf(line, sizeof(line), "Speed: %d / 7", app->scroll_speed);
                canvas_draw_str(canvas, 6, y, line);
            } else {
                canvas_draw_str(canvas, 6, y, scroll_labels[idx]);
            }
            canvas_set_color(canvas, ColorBlack);
        }
        break;
    }

    case SceneInfo: {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Display Info");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);

        char line[40];
        snprintf(line, sizeof(line), "Address: 0x%02X", app->detected_addr);
        canvas_draw_str(canvas, 6, 28, line);

        bool alive = ssd1306_detect(app->oled.i2c_addr);
        snprintf(line, sizeof(line), "Status: %s", alive ? "Connected" : "Lost!");
        canvas_draw_str(canvas, 6, 40, line);

        canvas_draw_str(canvas, 6, 52, "Size: 128x64");

        snprintf(line, sizeof(line), "Variant: %s",
            app->yellow_bar_height > 0 ? "Yellow-bar (16px)" : "Monochrome");
        canvas_draw_str(canvas, 6, 64, line);
        break;
    }

    case SceneClock: {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Clock + Status");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);

        DateTime dt;
        furi_hal_rtc_get_datetime(&dt);
        char time_str[12];
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, time_str);

        char date_str[16];
        snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", dt.year, dt.month, dt.day);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, date_str);

        uint8_t bat = furi_hal_power_get_pct();
        char bat_str[20];
        snprintf(bat_str, sizeof(bat_str), "Battery: %d%%%s", bat,
            furi_hal_power_is_charging() ? " [CHG]" : "");
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, bat_str);

        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "BACK to exit");
        break;
    }

    case ScenePlant:
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Desk Plant");
        canvas_draw_line(canvas, 0, 15, 128, 15);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "See OLED.");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Don't mess this up.");
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "BACK to exit menu");
        break;
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = (FuriMessageQueue*)ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

static void handle_plant(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyOk:
        // water the plant
        app->plant_water += 20;
        render_plant_oled(app);
        break;
    case InputKeyBack:
        app->scene = SceneMainMenu;
        app->cursor = 6;
        break;
    default:
        break;
    }
}

// -- Input handling --

static void handle_main_menu(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(app->cursor > 0) app->cursor--;
        break;
    case InputKeyDown:
        if(app->cursor < MAIN_MENU_COUNT - 1) app->cursor++;
        break;
    case InputKeyOk:
        switch(app->cursor) {
        case 0:
            app->scene = SceneClock;
            render_clock_oled(app);
            break;
        case 1: app->scene = ScenePatterns; app->cursor = 0; break;
        case 2: app->scene = SceneBrightness; break;
        case 3: app->scene = SceneCommands; app->cursor = 0; break;
        case 4: app->scene = SceneScrolling; app->cursor = 0; break;
        case 5: app->scene = SceneInfo; break;
        case 6: 
            app->scene = ScenePlant; 
            if(app->plant_water == 0 && app->plant_growth == 0) {
                app->plant_water = 50; // default start
            }
            render_plant_oled(app);
            break;
        }
        break;
    case InputKeyBack:
        app->running = false;
        break;
    default:
        break;
    }
}

static void handle_patterns(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(app->cursor > 0) app->cursor--;
        break;
    case InputKeyDown:
        if(app->cursor < PATTERN_COUNT - 1) app->cursor++;
        break;
    case InputKeyOk:
        if(app->cursor < PATTERN_COUNT) {
            pattern_funcs[app->cursor](app);
        }
        break;
    case InputKeyBack:
        app->scene = SceneMainMenu;
        app->cursor = 1;
        break;
    default:
        break;
    }
}

static void handle_brightness(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyLeft:
        if(app->brightness >= 5) app->brightness -= 5;
        else app->brightness = 0;
        ssd1306_set_contrast(&app->oled, app->brightness);
        break;
    case InputKeyRight:
        if(app->brightness <= 250) app->brightness += 5;
        else app->brightness = 255;
        ssd1306_set_contrast(&app->oled, app->brightness);
        break;
    case InputKeyBack:
        app->scene = SceneMainMenu;
        app->cursor = 2;
        break;
    default:
        break;
    }
}

static void handle_commands(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(app->cursor > 0) app->cursor--;
        break;
    case InputKeyDown:
        if(app->cursor < CMD_COUNT - 1) app->cursor++;
        break;
    case InputKeyOk:
        switch(app->cursor) {
        case 0:
            app->cmd_invert = !app->cmd_invert;
            ssd1306_set_invert(&app->oled, app->cmd_invert);
            break;
        case 1:
            app->cmd_flip_h = !app->cmd_flip_h;
            ssd1306_set_flip_h(&app->oled, app->cmd_flip_h);
            break;
        case 2:
            app->cmd_flip_v = !app->cmd_flip_v;
            ssd1306_set_flip_v(&app->oled, app->cmd_flip_v);
            break;
        case 3:
            app->cmd_all_on = !app->cmd_all_on;
            ssd1306_set_all_on(&app->oled, app->cmd_all_on);
            break;
        case 4:
            app->cmd_power = !app->cmd_power;
            ssd1306_power(&app->oled, !app->cmd_power);
            break;
        case 5:
            app->yellow_bar_height =
                (app->yellow_bar_height == 0) ? YELLOW_BAR_16 : YELLOW_BAR_NONE;
            break;
        }
        break;
    case InputKeyBack:
        app->scene = SceneMainMenu;
        app->cursor = 3;
        break;
    default:
        break;
    }
}

static void handle_scrolling(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(app->cursor > 0) app->cursor--;
        break;
    case InputKeyDown:
        if(app->cursor < SCROLL_COUNT - 1) app->cursor++;
        break;
    case InputKeyOk:
        switch(app->cursor) {
        case 0: ssd1306_scroll_h(&app->oled, false, 0, 7, app->scroll_speed); break;
        case 1: ssd1306_scroll_h(&app->oled, true, 0, 7, app->scroll_speed); break;
        case 2: ssd1306_scroll_hv(&app->oled, false, 0, 7, app->scroll_speed, 1); break;
        case 3: ssd1306_scroll_hv(&app->oled, true, 0, 7, app->scroll_speed, 1); break;
        case 4: ssd1306_scroll_stop(&app->oled); break;
        case 5:
            app->scroll_speed = (app->scroll_speed + 1) & 0x07;
            break;
        }
        break;
    case InputKeyLeft:
        if(app->cursor == 5 && app->scroll_speed > 0) app->scroll_speed--;
        break;
    case InputKeyRight:
        if(app->cursor == 5 && app->scroll_speed < 7) app->scroll_speed++;
        break;
    case InputKeyBack:
        ssd1306_scroll_stop(&app->oled);
        app->scene = SceneMainMenu;
        app->cursor = 4;
        break;
    default:
        break;
    }
}

static void handle_info(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress) return;
    if(ev->key == InputKeyBack) {
        app->scene = SceneMainMenu;
        app->cursor = 5;
    }
}

static void handle_clock(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress) return;
    if(ev->key == InputKeyBack) {
        app->scene = SceneMainMenu;
        app->cursor = 0;
    }
}

// -- Entry point --

int32_t ssd1306_app_main(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->running = true;
    app->brightness = 207; // default contrast
    app->cmd_power = false; // power off toggle (display starts ON)
    app->yellow_bar_height = YELLOW_BAR_NONE;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    // Try both addresses
    if(ssd1306_detect(SSD1306_ADDR_3C)) {
        app->detected_addr = 0x3C;
        app->detected = ssd1306_init(&app->oled, SSD1306_ADDR_3C);
    } else if(ssd1306_detect(SSD1306_ADDR_3D)) {
        app->detected_addr = 0x3D;
        app->detected = ssd1306_init(&app->oled, SSD1306_ADDR_3D);
    }

    if(app->detected) {
        // show initial test pattern
        ssd1306_clear(&app->oled);
        ssd1306_string(&app->oled, 16, 4, "SSD1306 Test");
        ssd1306_string(&app->oled, 10, 20, "Ready");
        ssd1306_string(&app->oled, 10, 36, "Use Flipper");
        ssd1306_string(&app->oled, 10, 46, "to navigate");
        ssd1306_rect(&app->oled, 0, 0, 128, 64);
        ssd1306_flush(&app->oled);
    }

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_callback, app);
    view_port_input_callback_set(vp, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent ev;
    while(app->running) {
        if(furi_message_queue_get(queue, &ev, 200) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);

            if(!app->detected) {
                // on any press, retry detection
                if(ev.type == InputTypePress) {
                    if(ev.key == InputKeyBack) {
                        app->running = false;
                    } else {
                        if(ssd1306_detect(SSD1306_ADDR_3C)) {
                            app->detected_addr = 0x3C;
                            app->detected = ssd1306_init(&app->oled, SSD1306_ADDR_3C);
                        } else if(ssd1306_detect(SSD1306_ADDR_3D)) {
                            app->detected_addr = 0x3D;
                            app->detected = ssd1306_init(&app->oled, SSD1306_ADDR_3D);
                        }
                    }
                }
            } else {
                switch(app->scene) {
                case SceneMainMenu: handle_main_menu(app, &ev); break;
                case ScenePatterns:  handle_patterns(app, &ev); break;
                case SceneBrightness: handle_brightness(app, &ev); break;
                case SceneCommands: handle_commands(app, &ev); break;
                case SceneScrolling: handle_scrolling(app, &ev); break;
                case SceneInfo:     handle_info(app, &ev); break;
                case SceneClock:    handle_clock(app, &ev); break;
                case ScenePlant:    handle_plant(app, &ev); break;
                }
            }

            furi_mutex_release(app->mutex);
            view_port_update(vp);
        }

        // periodic clock update (on message queue timeout, ~5 times/sec)
        if(app->detected && app->scene == SceneClock) {
            DateTime dt;
            furi_hal_rtc_get_datetime(&dt);
            if(dt.second != app->last_second) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                render_clock_oled(app);
                furi_mutex_release(app->mutex);
                view_port_update(vp); // refresh Flipper screen too
            }
        }

        // periodic plant update
        if(app->detected && app->scene == ScenePlant) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            
            // plant logic
            if (app->plant_water > 0 && app->plant_water <= 100) {
                app->plant_growth++;
            }
            app->plant_water--; // constantly drying or draining
            
            render_plant_oled(app);
            furi_mutex_release(app->mutex);
        }
    }

    // cleanup
    if(app->detected) {
        ssd1306_scroll_stop(&app->oled);
        ssd1306_clear(&app->oled);
        ssd1306_flush(&app->oled);
        ssd1306_power(&app->oled, false);
    }

    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
