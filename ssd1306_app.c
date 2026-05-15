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

    bool running;
    FuriMutex* mutex;
} App;

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
    "Test Patterns",
    "Brightness",
    "Display Commands",
    "Scrolling",
    "Display Info",
};
#define MAIN_MENU_COUNT 5

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
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = (FuriMessageQueue*)ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
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
        case 0: app->scene = ScenePatterns; app->cursor = 0; break;
        case 1: app->scene = SceneBrightness; break;
        case 2: app->scene = SceneCommands; app->cursor = 0; break;
        case 3: app->scene = SceneScrolling; app->cursor = 0; break;
        case 4: app->scene = SceneInfo; break;
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
        app->cursor = 0;
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
        app->cursor = 1;
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
        app->cursor = 2;
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
        app->cursor = 3;
        break;
    default:
        break;
    }
}

static void handle_info(App* app, InputEvent* ev) {
    if(ev->type != InputTypePress) return;
    if(ev->key == InputKeyBack) {
        app->scene = SceneMainMenu;
        app->cursor = 4;
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
                }
            }

            furi_mutex_release(app->mutex);
            view_port_update(vp);
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
