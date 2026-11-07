#include "esp_attr.h"
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_ui.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "src/draw/lv_image_decoder_private.h"   /* custom decoder */
#include "src/misc/lv_area_private.h"            /* lv_area_intersect, for the ring */

#include "muse_ble.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_voice.h"
#if CONFIG_MUSE_MODULE_LLM
#include "muse_conversation.h"
#include "muse_memory.h"
#include "muse_rich_text.h"
#include "muse_reader_layout.h"
#include "src/misc/lv_text_private.h"
#include "muse_module_llm.h"
#include "muse_startup.h"
#include "muse_native_voice.h"
#include "muse_wait_estimate.h"
#include "muse_storyteller_version.h"
#define HAS_MODULE_AUDIO true
#else
#define HAS_MODULE_AUDIO false
#endif
#include "muse_state.h"
#include "muse_text.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "muse_ui";

#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define RING_RANGE 1000
#define CAPTION_W 256           /* 16 columns of unscii_16, the width reply captions wrap to */
#define CAPTION_LINE_SPACE 2
#define ART_BLANK_ROWS 3        /* Muse's art never reaches the grid's bottom rows */
#define MINI_CELL_PX 2          /* Muse's grid cells over a reply that's read */
#define ANSWER_MS 300           /* Muse making room for a reply, and back */
#define SPEAKER_PX 64
#define SPEAKER_GROW_PX 8       /* how much the speaker button swells while held */
#define SPEAKER_HOLD_MS 400     /* LVGL's long press */

#define COLOR_DIM 0x8780a8
#define COLOR_CAPTION 0xd3ccff
#define COLOR_RING_BG 0x120d22
#define COLOR_METER_OFF 0x1a1433
#define COLOR_ACCENT 0x9c6dff
#define COLOR_DOT_OFF 0x362e58
#define COLOR_LIT 0xf0edff
#define SETTINGS_TICK_S 0.25f

/* Text on 128 px screens, as in the button menu (muse_menu.c). */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

/*
 * Two layouts: the full one (round 466 px or similar) with a progress ring,
 * level meter and settings tile, and a compact one for tiny screens (128 px)
 * where Muse fills the screen and text overlays it.
 */
static int s_w, s_h;
static bool s_small;
static bool s_tall;         /* compact, with room above and below Muse (StickS3) */
static int s_canvas_px;     /* Muse's size on screen */
static int s_dy;            /* full layout: offset from a 466 px tall screen */
static lv_indev_t *s_indev;
static lv_obj_t *s_tv;
/* True while the face tile is the visible tile. Updated in the LVGL task;
 * read from any task (single word, benign if briefly stale). */
static volatile bool s_face_active = true;
static lv_obj_t *s_face;

static void tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    s_face_active = (lv_tileview_get_tile_active(tv) == s_face);
}

/* From any task: the face tile is the currently visible tile. */
bool muse_ui_face_active(void)
{
    return s_face_active;
}
static lv_obj_t *s_settings;
static lv_obj_t *s_dots[2];
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_ble_icon;
static lv_obj_t *s_cover;
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_canvas;
static lv_obj_t *s_mic_icon;
static lv_obj_t *s_ptt_hint, *s_spk_hint, *s_pwr_hint;
#if LV_USE_SNAPSHOT
static volatile bool s_bench_demo;
void muse_ui_bench_demo(bool enabled)
{
    s_bench_demo = enabled;   /* display-only: never changes BLE identity/setup */
}
#endif
static lv_obj_t *s_ring;
static lv_obj_t *s_bar;     /* compact layout's stand-in for the ring */
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_name_lbl;    /* this gadget's own name, to tell it from the next one */
static lv_obj_t *s_power_lbl;
static lv_obj_t *s_caption_lbl;
static lv_obj_t *s_reply_lbl;   /* full layout: the reply's page while answering */
static lv_obj_t *s_meter[METER_SEGS];
static lv_obj_t *s_speaker;
static lv_obj_t *s_speaker_icon;
static lv_obj_t *s_aux_icon;
static lv_obj_t *s_image;   /* display.draw_url, over the face */
#if CONFIG_MUSE_WATCHER_CAMERA
static lv_obj_t *s_camera_hint;
#endif
static lv_image_dsc_t s_image_dsc;  /* its data is set once the image is shown */
/* The download writes the pixels without the display lock, so a big JPEG isn't
 * held up by each frame; this guards the buffer and what changed in it. */
static SemaphoreHandle_t s_image_mutex;
static uint16_t *s_image_buf;
static lv_area_t s_image_area;
static bool s_image_dirty;
static bool s_ready;

static float s_level;
static int s_shown_state = -1;
static const char *s_shown_name;
static const char *s_idle_name = "READY";   /* idle's label: set by the Wi-Fi state */
static int s_shown_lit = -1;
static uint32_t s_shown_accent;
static bool s_meter_visible = true;
static int s_ring_value = -1;
static uint32_t s_caption_version;
static float s_next_power_update;
static float s_next_settings_tick;
static bool s_dark;
static bool s_recorder_dark;
static atomic_bool s_recorder_wake;
static int64_t s_recorder_tap;
void muse_ui_recorder_wake(void){atomic_store(&s_recorder_wake,true);}
void muse_ui_recorder_screen_off(void){s_recorder_dark=true;}
static int s_brightness = -1;       /* last applied */
static int s_preview_brightness = -1;
static int s_shown_page = -1;
static int s_shown_speaker = -1;
static muse_mode_t s_last_mode = MUSE_MODE_COUNT;
static muse_mode_t s_last_epaper_pose = MUSE_MODE_COUNT;

/*
 * While Muse is thinking or speaking it shrinks to make room for the reply:
 * a little when the reply is heard, over a few lines to follow along, and
 * to the top when it's only read, over a page.
 */
typedef struct {
    int px, y;                /* Muse's size and centre */
    int cols, lines;          /* the reply's page */
    int w, h, top;            /* and where it goes */
    lv_text_align_t align;
    lv_obj_t *hides[4];       /* what it covers */
} answer_layout_t;

enum { ANSWER_HEARD, ANSWER_READ };
static answer_layout_t s_answers[2];
static int s_answer = -1;       /* the layout showing, or -1 */
static int s_page_for = -1;     /* the layout the reply's page is sized for */
static int s_big_y;             /* Muse's centre at full size */
static int s_muse_y;            /* and now */
static int s_from_px, s_from_y, s_to_px, s_to_y;

static const char *const MODE_NAMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = "WAKING UP",
    [MUSE_MODE_IDLE] = "READY",
    [MUSE_MODE_LISTENING] = "LISTENING",
    [MUSE_MODE_THINKING] = "THINKING",
    [MUSE_MODE_SPEAKING] = "SPEAKING",
    [MUSE_MODE_ERROR] = "ERROR",
    [MUSE_MODE_OFF] = "GOODBYE",
};

/*
 * Muse reaches LVGL as an image whose pixels are made on demand: a decoder
 * scales the 64 px grid up one strip of rows at a time, so the full-size frame
 * never sits in RAM and LVGL never runs its (slow) image transform.
 *
 * LVGL splits each refresh into a tile per draw unit and draws them at once,
 * each with its own open decoder, so each one takes its own strip: with one
 * shared strip, a tile blended rows the other had just scaled into it.
 */
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT

#define MUSE_PIXEL_MAX_PX 512   /* muse_pixel_set_size's cap */
#define DIRTY_RECTS_MAX 8       /* with the meter and labels, well inside LVGL's 32 areas */
#define DIRTY_MERGE_PX 2048     /* pixels that cost about one walk of the widget tree */

static lv_image_dsc_t s_muse_src;
static lv_draw_buf_t *s_strips[STRIPS];
static bool s_strip_busy[STRIPS];
static uint16_t *s_cells;       /* each cell's colour as last invalidated */
static uint16_t *s_cell_row;    /* one screen row of Muse */
static bool s_cells_valid;

static lv_result_t muse_dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_src) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_src.header;
    return LV_RESULT_OK;
}

/* LVGL holds its decoder lock around open and close. */
static lv_result_t muse_dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (!s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;   /* the rest of the work is in get_area */
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t muse_dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                                     const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(dsc->user_data, LV_COLOR_FORMAT_RGB565, w, y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale((uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t), full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

static void muse_dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;   /* kept for the next open */
        }
    }
}

static void muse_image_init(void)
{
    s_muse_src.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_src.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_src.header.w = s_canvas_px;
    s_muse_src.header.h = s_canvas_px;
    s_muse_src.header.stride = s_canvas_px * sizeof(uint16_t);
    s_muse_src.data = (const uint8_t *)&s_muse_src;   /* LVGL skips sources without data */
    muse_pixel_set_size(s_canvas_px);

    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(s_canvas_px, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        assert(s_strips[i]);
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    assert(dec);
    lv_image_decoder_set_info_cb(dec, muse_dec_info);
    lv_image_decoder_set_open_cb(dec, muse_dec_open);
    lv_image_decoder_set_get_area_cb(dec, muse_dec_get_area);
    lv_image_decoder_set_close_cb(dec, muse_dec_close);
    dec->name = "muse";

    s_cells = heap_caps_malloc(MUSE_PX_W * MUSE_PX_H * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_cell_row = heap_caps_malloc(MUSE_PIXEL_MAX_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!s_cells || !s_cell_row) {
        free(s_cells);
        free(s_cell_row);
        s_cells = s_cell_row = NULL;   /* redraw all of Muse every frame */
    }
}

/*
 * Most frames change only a few of Muse's cells, so only those are redrawn.
 * Each frame reads back one screen pixel of every cell (its first, which the
 * faint grid never dims) and compares it with the last frame's. A row's
 * changed cells make one span, and neighbouring spans merge while the pixels
 * drawn for nothing cost less than LVGL's walk of the widget tree for one more
 * area.
 */
static int32_t cell_px(int cell, int size)
{
    return (cell * size + MUSE_PX_W - 1) / MUSE_PX_W;   /* a cell's first screen pixel */
}

static int32_t rect_cells(const lv_area_t *r)
{
    return lv_area_get_width(r) * lv_area_get_height(r);
}

static void invalidate_muse(void)
{
    if (!s_cells) {
        lv_obj_invalidate(s_canvas);
        return;
    }
    int size = (int)s_muse_src.header.w;
    lv_area_t r[MUSE_PX_H];   /* in cells */
    int n = 0;
    for (int cy = 0; cy < MUSE_PX_H; cy++) {
        int32_t y = cell_px(cy, size);
        muse_pixel_scale(s_cell_row, size, 0, size - 1, y, y);
        uint16_t *last = &s_cells[cy * MUSE_PX_W];
        int x1 = -1, x2 = -1;
        for (int cx = 0; cx < MUSE_PX_W; cx++) {
            uint16_t c = s_cell_row[cell_px(cx, size)];
            if (c != last[cx] || !s_cells_valid) {
                last[cx] = c;
                x1 = x1 < 0 ? cx : x1;
                x2 = cx;
            }
        }
        if (x2 >= 0) {
            r[n++] = (lv_area_t){x1, cy, x2, cy};
        }
    }
    s_cells_valid = true;

    int64_t merge_cells = (int64_t)DIRTY_MERGE_PX * MUSE_PX_W * MUSE_PX_H / (size * size);
    while (n > 1) {
        int best = 0;
        int32_t best_cost = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t j = {LV_MIN(r[i].x1, r[i + 1].x1), r[i].y1, LV_MAX(r[i].x2, r[i + 1].x2), r[i + 1].y2};
            int32_t cost = rect_cells(&j) - rect_cells(&r[i]) - rect_cells(&r[i + 1]);
            if (cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (n <= DIRTY_RECTS_MAX && best_cost > merge_cells) {
            break;
        }
        r[best].x1 = LV_MIN(r[best].x1, r[best + 1].x1);
        r[best].x2 = LV_MAX(r[best].x2, r[best + 1].x2);
        r[best].y2 = r[best + 1].y2;
        memmove(&r[best + 1], &r[best + 2], (n - best - 2) * sizeof(r[0]));
        n--;
    }

    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {
            at.x1 + cell_px(r[i].x1, size),
            at.y1 + cell_px(r[i].y1, size),
            at.x1 + cell_px(r[i].x2 + 1, size) - 1,
            at.y1 + cell_px(r[i].y2 + 1, size) - 1,
        };
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

/* A microphone from primitives: LVGL's symbol font has none. */
static lv_obj_t *make_mic(lv_obj_t *parent, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    int stroke = size / 10 > 1 ? size / 10 : 1;

    lv_obj_t *head = lv_obj_create(box);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, size * 3 / 8, size * 9 / 16);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cradle = lv_arc_create(box);
    lv_obj_set_size(cradle, size * 5 / 8 + stroke, size * 5 / 8 + stroke);
    lv_arc_set_bg_angles(cradle, 0, 180);
    lv_obj_remove_style(cradle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cradle, 0, 0);
    lv_obj_set_style_arc_opa(cradle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cradle, stroke, LV_PART_MAIN);
    lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *stem = lv_obj_create(box);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, stroke, size * 7 / 8 - size * 5 / 8);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 5 / 8);

    lv_obj_t *base = lv_obj_create(box);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, size * 3 / 8, stroke);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, size * 7 / 8);
    return box;
}

static void color_mic(lv_obj_t *icon, uint32_t color)
{
    if (!icon) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(icon); i++) {
        lv_obj_t *part = lv_obj_get_child(icon, i);
        lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
        lv_obj_set_style_arc_color(part, lv_color_hex(color), LV_PART_MAIN);
    }
}

static void set_mic_color(uint32_t color)
{
    color_mic(s_mic_icon, color);   /* legacy face icon only */
}

static void on_touch_mic(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        muse_input_touch_talk(true);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        muse_input_touch_talk(false);
        break;
    default:
        break;
    }
}

/* Icons beside the physical buttons, in place of an instruction caption. */
static void build_button_icons(lv_obj_t *face)
{
    if (muse_board->rim_controls) return;   /* dedicated rim hints below */
    const muse_button_hint_t *t = &muse_board->talk_hint, *a = &muse_board->aux_hint;
    s_mic_icon = make_mic(face, s_tall ? 24 : s_small ? 12 : 26);
    lv_obj_align(s_mic_icon, t->align, t->x, t->y);
    set_mic_color(COLOR_DIM);
    if (muse_board->touch_talk) {
        lv_obj_add_flag(s_mic_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_mic_icon, 12);
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_mic_icon); i++) {
            lv_obj_remove_flag(lv_obj_get_child(s_mic_icon, i), LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_add_event_cb(s_mic_icon, on_touch_mic, LV_EVENT_ALL, NULL);
    }

    /* Without touch the aux button opens the menu rather than sleeping. A board
     * that leaves aux_hint out has no button to put an icon beside. */
    if (a->align == LV_ALIGN_DEFAULT) {
        return;
    }
    s_aux_icon = make_label(face, s_small ? &lv_font_montserrat_14 : &lv_font_montserrat_28, COLOR_DIM);
    lv_label_set_text(s_aux_icon, muse_board->touch ? LV_SYMBOL_POWER : LV_SYMBOL_LIST);
    lv_obj_align(s_aux_icon, a->align, a->x, a->y);
}

static void on_canvas_clicked(lv_event_t *e)
{
    (void)e;
    muse_state_make_happy();
}

static const lv_font_t *font_pick(const lv_font_t *full, const lv_font_t *compact)
{
    return s_small ? compact : full;
}

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#endif

/* unscii-16 for captions and replies; with CONFIG_MUSE_CJK_FONT, a copy that
 * falls back to Unifont's 16x16 CJK, the same cell, for what unscii lacks. */
static const lv_font_t *caption_font(void)
{
#if CONFIG_MUSE_CJK_FONT
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_unscii_16;
        font.fallback = &muse_font_cjk_16;
    }
    return &font;
#else
    return &lv_font_unscii_16;
#endif
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static void show_speaker(bool on)
{
    /* Off is the one that stands out, like a lit flashlight button. */
    lv_obj_set_style_bg_color(s_speaker, lv_color_hex(on ? COLOR_METER_OFF : COLOR_LIT), 0);
    lv_obj_set_style_text_color(s_speaker_icon, lv_color_hex(on ? COLOR_DIM : 0x000000), 0);
    lv_label_set_text(s_speaker_icon, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    s_shown_speaker = on;
}

static void set_speaker_size(void *obj, int32_t px)
{
    lv_obj_set_size(obj, px, px);
}

/* Swells over the long press, so the toggle lands as it reaches full size. */
static void speaker_grow(bool grow)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_speaker);
    lv_anim_set_exec_cb(&a, set_speaker_size);
    lv_anim_set_values(&a, lv_obj_get_width(s_speaker), SPEAKER_PX + (grow ? SPEAKER_GROW_PX : 0));
    lv_anim_set_duration(&a, grow ? SPEAKER_HOLD_MS : 150);
    lv_anim_start(&a);
}

/* Touch and hold to toggle, like the lock screen's flashlight; a tap only says so. */
static void on_speaker_event(lv_event_t *e)
{
    bool on = muse_settings_speaker_on();
    bool idle = muse_state_mode(NULL) == MUSE_MODE_IDLE;   /* don't cover a reply's captions */
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        speaker_grow(true);
        break;
    case LV_EVENT_LONG_PRESSED:
        muse_settings_set_speaker_on(!on);
        show_speaker(!on);
        if (idle) {
            muse_state_set_caption(on ? "SPEAKER OFF" : "SPEAKER ON");
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (idle) {
            muse_state_set_caption(on ? "HOLD TO MUTE" : "HOLD TO UNMUTE");
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        speaker_grow(false);
        break;
    default:
        break;
    }
}

/* Upper left by the status icons, where a thumb finds it without covering the reply. */
static void build_speaker(lv_obj_t *face, int x, int y)
{
    s_speaker = lv_obj_create(face);
    lv_obj_remove_style_all(s_speaker);
    lv_obj_set_size(s_speaker, SPEAKER_PX, SPEAKER_PX);
    lv_obj_set_style_radius(s_speaker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_speaker, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_speaker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_speaker, 12);   /* a fingertip is bigger than the circle */
    static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_SHORT_CLICKED,
                                              LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_obj_add_event_cb(s_speaker, on_speaker_event, EVENTS[i], NULL);
    }
    s_speaker_icon = make_label(s_speaker, &lv_font_montserrat_28, COLOR_DIM);
    lv_obj_center(s_speaker_icon);
    lv_obj_align(s_speaker, LV_ALIGN_CENTER, x, y);
    show_speaker(muse_settings_speaker_on());
}

static void set_canvas_px(int px)
{
    if ((int)s_muse_src.header.w == px) {
        return;
    }
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    muse_pixel_set_size(px);
    lv_image_set_src(s_canvas, &s_muse_src);   /* picks up the new size */
}

static void move_muse_t(void *obj, int32_t t)
{
    (void)obj;
    set_canvas_px(s_from_px + (s_to_px - s_from_px) * t / 256);
    s_muse_y = s_from_y + (s_to_y - s_from_y) * t / 256;
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_muse_y);
}

/* Eases Muse to `px` centred at `y`, from wherever it is now. */
static void move_muse(int px, int y)
{
    s_from_px = s_muse_src.header.w;
    s_from_y = s_muse_y;
    s_to_px = px;
    s_to_y = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_canvas);
    lv_anim_set_exec_cb(&a, move_muse_t);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, ANSWER_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Switches to answer layout `which`, or back to the usual one for -1. */
static void set_answer(int which)
{
    const answer_layout_t *was = s_answer >= 0 ? &s_answers[s_answer] : NULL;
    const answer_layout_t *l = which >= 0 ? &s_answers[which] : NULL;
    s_answer = which;
    const size_t n = sizeof(s_answers[0].hides) / sizeof(s_answers[0].hides[0]);
    for (size_t i = 0; was && i < n; i++) {
        if (was->hides[i]) {
            lv_obj_remove_flag(was->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = 0; l && i < n; i++) {
        if (l->hides[i]) {
            lv_obj_add_flag(l->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!muse_board->round && s_ring) {
        lv_obj_set_flag(s_ring, LV_OBJ_FLAG_HIDDEN, l != NULL);   /* the reply runs past a rectangle's ring */
    }
    if (l) {
        lv_obj_set_size(s_reply_lbl, l->w, l->h);
        lv_obj_align(s_reply_lbl, LV_ALIGN_CENTER, 0, l->top + l->h / 2);
        lv_obj_set_style_text_align(s_reply_lbl, l->align, 0);
    }
    move_muse(l ? l->px : s_canvas_px, l ? l->y : s_big_y);
}

/* Whether a reply `w` px wide fits across the screen `y` px from the centre. */
static bool fits_across(int w, int y, int ring_in)
{
    if (!muse_board->round) {
        return w <= s_w - 16;
    }
    int r = ring_in - 6;
    return w * w / 4 + y * y <= r * r;
}

/* How far down a reply `w` px wide can reach: inside the ring, above the page dots. */
static int reply_bottom(int w, int ring_in)
{
    int bottom = s_h / 2 - 28;
    if (muse_board->round) {
        int r = ring_in - 6;
        int chord = w / 2 < r ? (int)sqrtf((float)(r * r - w * w / 4)) : 0;
        bottom = chord < bottom ? chord : bottom;
    }
    return bottom;
}

static void set_reply_box(answer_layout_t *l, int cols, int lines, int top, int cw, int pitch)
{
    l->cols = cols;
    l->lines = lines;
    l->w = cols * cw;
    l->h = lines * pitch - CAPTION_LINE_SPACE;
    l->top = top;
}

/* The hint icons a layout's reply would cover go while it's up. */
static void add_hides(answer_layout_t *l, int n)
{
    lv_area_t box = {
        .x1 = s_w / 2 - l->w / 2, .y1 = s_h / 2 + l->top,
        .x2 = s_w / 2 + l->w / 2 - 1, .y2 = s_h / 2 + l->top + l->h - 1,
    };
    lv_obj_t *const hints[] = { s_mic_icon, s_aux_icon };
    for (size_t i = 0; i < 2; i++) {
        if (!hints[i]) {
            continue;   /* no aux icon on this board */
        }
        lv_area_t a;
        lv_obj_get_coords(hints[i], &a);
        if (a.x1 <= box.x2 && a.x2 >= box.x1 && a.y1 <= box.y2 && a.y2 >= box.y1) {
            l->hides[n++] = hints[i];
        }
    }
}

/*
 * The answer layouts, both with Muse centred. Heard: Muse a size smaller,
 * where it was if there's room, over three lines at the bottom. Read: Muse
 * small under the status line, and under it and the speaker button the
 * biggest page of reply text that fits inside the ring.
 */
static void build_answer(lv_obj_t *face, int ring_in)
{
    int spk_r = (SPEAKER_PX + SPEAKER_GROW_PX) / 2;
    int spk_x = -s_w / 2 + 8 + spk_r, spk_y = -s_h / 2 + 8 + spk_r;
    if (muse_board->round) {
        spk_y = -ring_in * 5 / 8;
        int d = ring_in - spk_r - 4;   /* just inside the ring, even when swollen */
        spk_x = -(int)sqrtf((float)(d * d - spk_y * spk_y));
    }
    const lv_font_t *font = caption_font();
    int cw = lv_font_get_glyph_width(font, 'M', ' ');
    int pitch = lv_font_get_line_height(font) + CAPTION_LINE_SPACE;

    answer_layout_t *l = &s_answers[ANSWER_HEARD];
    int cell = s_canvas_px / MUSE_PX_W - 1;
    cell = cell > MINI_CELL_PX ? cell : MINI_CELL_PX;
    l->px = MUSE_PX_W * cell;
    l->y = s_big_y;
    l->align = LV_TEXT_ALIGN_CENTER;
    int h = 3 * pitch - CAPTION_LINE_SPACE;
    int art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * cell;
    set_reply_box(l, CAPTION_W / cw, 3, reply_bottom(CAPTION_W, ring_in) - h, cw, pitch);
    for (int c = 24; c > l->cols; c--) {   /* wider if it still clears Muse */
        int top = reply_bottom(c * cw, ring_in) - h;
        if (top >= art_bottom + 6 && fits_across(c * cw, top, ring_in)) {
            set_reply_box(l, c, 3, top, cw, pitch);
            break;
        }
    }
    if (l->top < art_bottom + 6) {
        l->y -= art_bottom + 6 - l->top;   /* no room under Muse: it moves up */
    }

    l = &s_answers[ANSWER_READ];
    int status_bottom = (muse_board->rim_controls ? 40 : 20 + s_dy) + 16 - s_h / 2;
    l->px = MUSE_PX_W * MINI_CELL_PX;
    l->y = status_bottom + 2 + l->px / 2;   /* its sparkles clear of the status line */
    l->align = LV_TEXT_ALIGN_LEFT;
    art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * MINI_CELL_PX;
    int top = (art_bottom > spk_y + spk_r ? art_bottom : spk_y + spk_r) + 8;
    set_reply_box(l, muse_board->rim_controls ? 13 : 16, 2, top, cw, pitch);
    /* The widest page isn't the biggest: a round screen narrows towards the bottom. */
    /* Keep read replies in a central column clear of dedicated rim hints. */
    int max_cols = muse_board->rim_controls ? 13 : 24;
    for (int c = 12; c <= max_cols && fits_across(c * cw, top, ring_in); c++) {
        int n = (reply_bottom(c * cw, ring_in) - top + CAPTION_LINE_SPACE) / pitch;
        /* A third of the caption spare for characters wider than a byte. */
        while ((c + 1) * n > MUSE_CAPTION_MAX * 2 / 3) {
            n--;
        }
        if (c * n > l->cols * l->lines) {
            set_reply_box(l, c, n, top, cw, pitch);
        }
    }
    ESP_LOGI(TAG, "reply pages: %d x %d heard, %d x %d read", s_answers[ANSWER_HEARD].cols,
             s_answers[ANSWER_HEARD].lines, l->cols, l->lines);

    s_reply_lbl = make_label(face, font, COLOR_CAPTION);
    lv_obj_set_style_text_line_space(s_reply_lbl, CAPTION_LINE_SPACE, 0);
    /* Pages come wrapped to fit; the transcript while thinking doesn't. */
    lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_reply_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN);

    if (muse_board->touch && !muse_board->rim_controls) {
        build_speaker(face, spk_x, spk_y);
    }

    /* Out of the way while a page is up: the mode name, where Muse was. */
    lv_obj_update_layout(face);
    s_answers[ANSWER_READ].hides[0] = s_state_lbl;
    s_answers[ANSWER_READ].hides[1] = s_name_lbl;   /* the reply takes the top too */
    add_hides(&s_answers[ANSWER_READ], 2);
    add_hides(&s_answers[ANSWER_HEARD], 0);
}

/*
 * A full circle's arc masks every pixel of every row it's asked to draw, hole
 * and corners included, which costs a full-screen redraw (a swipe) several ms.
 * So the ring is drawn a slab of rows at a time, each only as wide as the ring
 * is there: where a slab crosses the hole, a left piece and a right one.
 */
#define RING_SLAB_ROWS 52

static void on_ring_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_area_t c;
    lv_obj_get_coords(s_ring, &c);
    int32_t cx = (c.x1 + c.x2) / 2;
    int32_t cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;                                     /* with antialiasing */
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(s_ring, LV_PART_MAIN) - 2;   /* without */
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += RING_SLAB_ROWS) {
        int32_t y2 = LV_MIN(y + RING_SLAB_ROWS - 1, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0;
        int32_t iw = far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0;
        lv_area_t pieces[2] = {
            { cx - ow, y, iw ? cx - iw : cx + ow, y2 },
            { cx + iw, y, cx + ow, y2 },
        };
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) {
                lv_obj_event_base(NULL, e);   /* the arc's own drawing */
            }
        }
    }
    layer->_clip_area = clip;
}

#if CONFIG_MUSE_MODULE_LLM
static lv_obj_t *s_conversation, *s_conversation_body, *s_conversation_label, *s_conversation_title;
static lv_obj_t *s_conversation_header, *s_conversation_latest_button, *s_conversation_follow_button, *s_conversation_follow_label;
static lv_obj_t *s_loading_panel,*s_loading_bar,*s_loading_text,*s_loading_note;
static bool s_loading_visible;
static muse_wait_view_t s_loading_view;
static char *s_conversation_text;
static char *s_rich_text;
static uint16_t *s_rich_positions;
static lv_font_t s_reader_font;
static muse_rich_meta_t *s_rich_meta;
typedef struct {int32_t x1,y1,x2,y2;uint8_t style;} reader_decoration_t;
static reader_decoration_t *s_reader_decor;
static unsigned s_reader_decor_count;
static bool s_reader_decor_dirty;
static muse_reader_point_t *s_reader_points;
static size_t s_reader_point_cap,s_reader_point_count;
static bool s_reader_layout_valid,s_reader_layout_dirty=true;
static lv_obj_t *s_reader_spacer;
static const char *s_reader_full_text="";
static char *s_reader_window[2];
static unsigned s_reader_window_slot;
static uint32_t s_reader_window_start=UINT32_MAX,s_reader_window_end;
static int32_t s_reader_window_top;
static uint32_t s_reader_selection_start=LV_LABEL_TEXT_SELECTION_OFF,s_reader_selection_end=LV_LABEL_TEXT_SELECTION_OFF;
static void reader_window_refresh(void);
static void reader_select_start(uint32_t index);
static void reader_select_end(uint32_t index);

static int32_t s_reader_line_height=20;
static volatile uint32_t s_reader_frames,s_reader_phase,s_reader_frame_at,s_reader_layout_fallbacks;
static volatile uint32_t s_display_event,s_display_event_at;
static void reader_display_event(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);
    if(code>=LV_EVENT_REFR_START && code<=LV_EVENT_FLUSH_WAIT_FINISH){s_display_event=(uint32_t)code;s_display_event_at=(uint32_t)(esp_timer_get_time()/1000);}
}
static bool reader_layout_ensure(void);
static void reader_letter_pos(uint32_t index,lv_point_t *point);
static uint32_t reader_letter_on(const lv_point_t *point);

static void reader_draw(lv_event_t *e);
static void reader_link(lv_event_t *e);

LV_FONT_DECLARE(muse_reader_bold_font);
static void conversation_render_style(const char *raw,bool bold,bool code,unsigned initial_style)
{
    if(s_rich_meta)s_rich_meta->initial_style=(uint8_t)initial_style;
    if(s_rich_text && s_rich_positions && muse_rich_render_full(raw,s_rich_text,MUSE_RICH_TEXT_CAP,s_rich_positions,MUSE_RICH_RAW_CAP+1,bold,code,s_rich_meta))
        {s_reader_full_text=s_rich_text;s_reader_decor_dirty=true;s_reader_layout_dirty=true;s_reader_layout_valid=false;s_reader_window_start=UINT32_MAX;}
    else {s_reader_full_text="Text could not be displayed.";s_reader_decor_dirty=true;s_reader_layout_dirty=true;s_reader_layout_valid=false;s_reader_window_start=UINT32_MAX;}
}

static void conversation_render(const char *raw){conversation_render_style(raw,false,false,0);}
static lv_obj_t *s_archive_footer,*s_archive_rows[5],*s_archive_more[5],*s_archive_save_label;
static bool s_archive_visible;
static char *s_archive_text;
EXT_RAM_BSS_ATTR static muse_memory_view_t s_archive_view;
static void archive_tick(void);
static lv_obj_t *s_library_dialog,*s_library_title,*s_library_note,*s_library_use,*s_library_another;
static char s_library_chat[33],s_library_name[96],s_library_token[25];
static unsigned s_library_variant;
static int s_library_mode;
static bool s_library_waiting;
static uint32_t s_library_action_seen;
static void library_dialog(int mode);
static void archive_row_visible(unsigned i,bool visible)
{
    if(visible){lv_obj_remove_flag(s_archive_rows[i],LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(s_archive_more[i],LV_OBJ_FLAG_HIDDEN);}
    else{lv_obj_add_flag(s_archive_rows[i],LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_archive_more[i],LV_OBJ_FLAG_HIDDEN);}
}

static void library_close(void)
{
    if(s_library_dialog)lv_obj_delete(s_library_dialog);
    s_library_dialog=NULL;s_library_use=NULL;s_library_another=NULL;s_library_title=NULL;s_library_note=NULL;s_library_mode=0;s_library_waiting=false;
}
static void library_action(int action)
{
    muse_state_poke();
    if(action==23){library_close();return;}
    if(s_library_waiting)return;
    if(action==20){char cid[33];strlcpy(cid,s_library_chat,33);library_close();muse_memory_action("page",cid,0);return;}
    if(action>=23 && action<=25){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_bench_recorder_sleep(action==24?-1:action==25?1:0);printf("@recorder_sleep {\"enforced\":%s,\"held\":%s,\"asleep\":%s}\n",muse_settings_recorder_sleep_enforced()?"true":"false",muse_settings_ui_recorder_keep_awake()?"true":"false",muse_state_asleep()?"true":"false");fflush(stdout);}
        if(action==22){library_dialog(2);return;}
    if(action==21 || action==26){
        if(action==21)s_library_variant=0;else s_library_variant++;
        s_library_token[0]=0;library_dialog(3);
        s_library_waiting=muse_memory_action("suggest",s_library_chat,s_library_variant);
    }else if(action==24)s_library_waiting=muse_memory_action("delete",s_library_chat,0);
    else if(action==25 && s_library_token[0])s_library_waiting=muse_memory_rename(s_library_chat,s_library_token);
    if(s_library_note)lv_label_set_text(s_library_note,s_library_waiting?"Working locally...":"Finish the reply, then try again.");
}
static void library_click(lv_event_t *e){library_action((int)(intptr_t)lv_event_get_user_data(e));}
static lv_obj_t *library_button(const char *text,int action,int x,int y,int width,bool danger)
{
    lv_obj_t *b=lv_button_create(s_library_dialog);lv_obj_remove_style_all(b);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,width,36);
    lv_obj_set_style_radius(b,9,0);lv_obj_set_style_bg_color(b,lv_color_hex(danger?0x803253:0x4c2f6e),0);lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
    lv_obj_add_event_cb(b,library_click,LV_EVENT_CLICKED,(void *)(intptr_t)action);
    lv_obj_t *label=make_label(b,&lv_font_montserrat_14,0xf3edff);lv_label_set_text(label,text);lv_obj_center(label);return b;
}
static void library_dialog(int mode)
{
    library_close();s_library_mode=mode;s_library_action_seen=s_archive_view.action_version;
    s_library_dialog=lv_obj_create(lv_layer_top());lv_obj_set_size(s_library_dialog,306,220);lv_obj_center(s_library_dialog);
    lv_obj_set_style_pad_all(s_library_dialog,12,0);lv_obj_remove_flag(s_library_dialog,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_library_dialog,lv_color_hex(0x191028),0);lv_obj_set_style_border_color(s_library_dialog,lv_color_hex(0x9c6aff),0);lv_obj_set_style_radius(s_library_dialog,14,0);
    lv_obj_t *heading=make_label(s_library_dialog,&lv_font_montserrat_14,0xc298ff);lv_obj_set_pos(heading,0,0);
    lv_label_set_text(heading,mode==1?"Manage conversation":mode==2?"Delete this conversation?":"Title / free & offline");
    s_library_title=make_label(s_library_dialog,&lv_font_montserrat_16,0xf3edff);lv_obj_set_pos(s_library_title,0,28);lv_obj_set_width(s_library_title,278);lv_label_set_long_mode(s_library_title,LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_library_title,mode==3?"Finding a title...":s_library_name);
    s_library_note=make_label(s_library_dialog,&lv_font_montserrat_14,0xbaa6ce);lv_obj_set_pos(s_library_note,0,91);lv_obj_set_width(s_library_note,278);
    lv_label_set_text(s_library_note,mode==1?"Only this chat will be changed.":mode==2?"Deletes its saved text on this device.":"Preview first. Your chat stays intact.");
    if(mode==1){library_button("Open",20,0,118,133,false);library_button("New title",21,143,118,133,false);library_button("Delete",22,0,160,133,true);library_button("Back",23,143,160,133,false);}
    else if(mode==2){library_button("Keep chat",23,0,160,133,false);library_button("Delete",24,143,160,133,true);}
    else{s_library_use=library_button("Use title",25,0,160,88,false);lv_obj_add_state(s_library_use,LV_STATE_DISABLED);s_library_another=library_button("Another",26,94,160,88,false);library_button("Back",23,188,160,88,false);}
}


static uint32_t s_conversation_version;
static bool s_transport_visible;
static unsigned s_conversation_size_mode = 1; /* 0 two lines, 1 card, 2 full */
static bool s_conversation_follow = true, s_conversation_scrolling;
static uint32_t s_conversation_latest_char, s_conversation_word_seen = UINT32_MAX;
static int s_conversation_follow_mode = -1;
static bool s_conversation_preview;
static int32_t s_conversation_latest_y;
#if LV_USE_SNAPSHOT
static volatile int s_conversation_bench = -1;
static volatile bool s_reader_stress_pending,s_reader_stress_cancel;
static bool s_reader_stress_active;
static unsigned s_reader_stress_index,s_reader_stress_mode,s_reader_stress_steps;
void muse_ui_bench_reader_stress(void){s_reader_stress_pending=true;}

void muse_ui_bench_conversation(int mode) { s_conversation_bench = mode;if(mode==0)s_reader_stress_cancel=true; }
#endif

static size_t reader_next_line(const char *text,size_t len,int32_t width,void *context)
{
    (void)context;lv_text_attributes_t a={0};a.max_width=width;
    a.line_space=lv_obj_get_style_text_line_space(s_conversation_label,0);a.letter_space=lv_obj_get_style_text_letter_space(s_conversation_label,0);
    return lv_text_get_next_line(text,(uint32_t)len,&s_reader_font,NULL,&a);
}
static int32_t reader_glyph_width(uint32_t cp,uint32_t next,void *context)
{
    (void)context;return lv_font_get_glyph_width(&s_reader_font,cp,next)+lv_obj_get_style_text_letter_space(s_conversation_label,0);
}
static bool reader_layout_ensure(void)
{
    if(!s_reader_layout_dirty)return s_reader_layout_valid;
    s_reader_phase=10;
    lv_obj_update_layout(s_conversation_label);
    int32_t width=lv_obj_get_content_width(s_conversation_label);
    if(width<1){s_reader_layout_valid=false;return false;}
    const char *text=s_reader_full_text;
    size_t needed=2;for(const unsigned char *p=(const unsigned char *)text;*p;p++)if((*p&0xc0)!=0x80)needed++;
    if(needed>s_reader_point_cap){
        muse_reader_point_t *points=heap_caps_realloc(s_reader_points,needed*sizeof(*points),MUSE_BIG_CAPS);
        if(!points){s_reader_layout_valid=false;s_reader_layout_dirty=false;return false;}
        s_reader_points=points;s_reader_point_cap=needed;
    }
    unsigned fallback=0;s_reader_line_height=lv_font_get_line_height(&s_reader_font)+lv_obj_get_style_text_line_space(s_conversation_label,0);
    if(s_reader_line_height<1)s_reader_line_height=20;
    s_reader_layout_valid=muse_reader_layout_build(text,width,s_reader_points,s_reader_point_cap,&s_reader_point_count,&fallback,reader_next_line,reader_glyph_width,NULL);
    s_reader_layout_fallbacks+=fallback;s_reader_layout_dirty=false;
    if(s_reader_layout_valid && s_reader_spacer){
        int32_t height=((int32_t)s_reader_points[s_reader_point_count].line+1)*s_reader_line_height;
        lv_obj_set_height(s_reader_spacer,LV_MAX(height,1));s_reader_window_start=UINT32_MAX;
    }
    return s_reader_layout_valid;
}
static void reader_letter_pos(uint32_t index,lv_point_t *point)
{
    *point=(lv_point_t){0,0};if(!reader_layout_ensure())return;
    if(index>s_reader_point_count)index=(uint32_t)s_reader_point_count;
    point->x=s_reader_points[index].x;point->y=(int32_t)s_reader_points[index].line*s_reader_line_height;
}
static uint32_t reader_letter_on(const lv_point_t *point)
{
    if(!reader_layout_ensure())return 0;
    return (uint32_t)muse_reader_layout_hit(s_reader_points,s_reader_point_count,point->x,point->y,s_reader_line_height);
}
void muse_ui_reader_diagnostic(muse_ui_reader_diag_t *out)
{
    if(!out)return;
    out->frames=s_reader_frames;out->phase=s_reader_phase;out->age_ms=(uint32_t)(esp_timer_get_time()/1000)-s_reader_frame_at;
    out->chars=(uint32_t)s_reader_point_count;out->layout_fallbacks=s_reader_layout_fallbacks;out->display_event=s_display_event;out->display_event_age_ms=(uint32_t)(esp_timer_get_time()/1000)-s_display_event_at;
}
static uint32_t reader_line_start(unsigned line)
{
    size_t lo=0,hi=s_reader_point_count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(s_reader_points[mid].line<line)lo=mid+1;else hi=mid;}
    return (uint32_t)lo;
}
static size_t reader_byte_at(uint32_t character)
{
    size_t byte=0,bytes=strlen(s_reader_full_text);uint32_t index=0;
    while(byte<bytes && index<character){size_t n;muse_reader_decode(s_reader_full_text+byte,bytes-byte,&n);byte+=n;index++;}
    return byte;
}
static void reader_selection_apply(void)
{
    if(s_reader_window_start==UINT32_MAX)return;
    if(s_reader_selection_start==LV_LABEL_TEXT_SELECTION_OFF || s_reader_selection_end==LV_LABEL_TEXT_SELECTION_OFF || s_reader_selection_end<=s_reader_window_start || s_reader_selection_start>=s_reader_window_end){
        if(lv_label_get_text_selection_start(s_conversation_label)!=LV_LABEL_TEXT_SELECTION_OFF)lv_label_set_text_selection_start(s_conversation_label,LV_LABEL_TEXT_SELECTION_OFF);
        if(lv_label_get_text_selection_end(s_conversation_label)!=LV_LABEL_TEXT_SELECTION_OFF)lv_label_set_text_selection_end(s_conversation_label,LV_LABEL_TEXT_SELECTION_OFF);
        return;
    }
    uint32_t begin=s_reader_selection_start<s_reader_window_start?0:s_reader_selection_start-s_reader_window_start;
    uint32_t end=(s_reader_selection_end>s_reader_window_end?s_reader_window_end:s_reader_selection_end)-s_reader_window_start;
    /* LVGL selection setters invalidate the whole label even when unchanged. */
    if(lv_label_get_text_selection_start(s_conversation_label)!=begin)lv_label_set_text_selection_start(s_conversation_label,begin);
    if(lv_label_get_text_selection_end(s_conversation_label)!=end)lv_label_set_text_selection_end(s_conversation_label,end);
}
static void reader_select_start(uint32_t index){s_reader_selection_start=index;reader_selection_apply();}
static void reader_select_end(uint32_t index){s_reader_selection_end=index;reader_selection_apply();}
static void reader_window_refresh(void)
{
    if(!s_reader_window[0] || !s_reader_window[1] || !s_reader_spacer || !reader_layout_ensure())return;
    int32_t top=lv_obj_get_scroll_y(s_conversation_body);if(top<0)top=0;
    unsigned first=(unsigned)(top/s_reader_line_height),rows=(unsigned)(lv_obj_get_height(s_conversation_body)/s_reader_line_height)+3;
    uint32_t start=reader_line_start(first?first-1:0),end=reader_line_start(first+rows+1);
    if(start==s_reader_window_start && end==s_reader_window_end){reader_selection_apply();return;}
    size_t begin_byte=reader_byte_at(start),end_byte=reader_byte_at(end),length=end_byte-begin_byte;
    if(length>=8192){
        end=start;end_byte=begin_byte;size_t bytes=strlen(s_reader_full_text);
        while(end<s_reader_point_count && end_byte<bytes){size_t n;muse_reader_decode(s_reader_full_text+end_byte,bytes-end_byte,&n);if(end_byte+n-begin_byte>=8192)break;end_byte+=n;end++;}
        length=end_byte-begin_byte;
    }
    s_reader_window_slot^=1;char *window=s_reader_window[s_reader_window_slot];memcpy(window,s_reader_full_text+begin_byte,length);window[length]=0;
    s_reader_window_start=start;s_reader_window_end=end;
    s_reader_window_top=(int32_t)s_reader_points[start].line*s_reader_line_height;
    lv_obj_set_y(s_conversation_label,s_reader_window_top);lv_label_set_text(s_conversation_label,window);reader_selection_apply();
}

static void reader_decor_build(void)
{
    s_reader_decor_count=0;s_reader_decor_dirty=false;
    if(!s_rich_meta || !s_reader_decor)return;
    unsigned at=0,count=s_rich_meta->count;int width=lv_obj_get_width(s_conversation_label);
    int height=lv_font_get_line_height(&s_reader_font)+lv_obj_get_style_text_line_space(s_conversation_label,0);
    while(at<count && s_reader_decor_count<1024){
        uint8_t flags=s_rich_meta->styles[at]&(MUSE_RICH_STRIKE|MUSE_RICH_CODE|MUSE_RICH_LINK|MUSE_RICH_QUOTE);
        unsigned end=at+1;while(end<count && (s_rich_meta->styles[end]&(MUSE_RICH_STRIKE|MUSE_RICH_CODE|MUSE_RICH_LINK|MUSE_RICH_QUOTE))==flags)end++;
        if(flags){
            lv_point_t a,z;reader_letter_pos(at,&a);reader_letter_pos(end,&z);
            for(int y=a.y;y<=z.y && s_reader_decor_count<1024;y+=height){
                int left=y==a.y?a.x:0,right=y==z.y?z.x:width;
                if(right>left){reader_decoration_t *d=&s_reader_decor[s_reader_decor_count++];d->x1=left;d->x2=right;d->y1=y;d->y2=y+lv_font_get_line_height(&s_reader_font);d->style=flags;}
            }
        }at=end;
    }
}
static void reader_draw(lv_event_t *e)
{
    if(!s_rich_meta || !s_reader_decor || s_reader_decor_dirty)return;
    /* Drawing only consumes cached geometry; it never requests layout. */
    lv_area_t origin;lv_obj_get_coords(s_conversation_label,&origin);origin.y1-=s_reader_window_top;lv_layer_t *layer=lv_event_get_layer(e);
    bool before=lv_event_get_code(e)==LV_EVENT_DRAW_MAIN_BEGIN;
    s_reader_phase=before?20:21;
    lv_area_t viewport;lv_obj_get_coords(s_conversation_body,&viewport);
    for(unsigned i=0;i<s_reader_decor_count;i++){
        reader_decoration_t *d=&s_reader_decor[i];lv_area_t a={origin.x1+d->x1,origin.y1+d->y1,origin.x1+d->x2-1,origin.y1+d->y2-1};
        if(a.y2<viewport.y1 || a.y1>viewport.y2)continue;
        lv_draw_rect_dsc_t rect;lv_draw_rect_dsc_init(&rect);rect.bg_opa=LV_OPA_COVER;
        if(before && (d->style&MUSE_RICH_CODE)){rect.bg_color=lv_color_hex(0x2e2042);rect.radius=3;lv_draw_rect(layer,&rect,&a);}
        if(before && (d->style&MUSE_RICH_QUOTE)){lv_area_t bar={origin.x1, a.y1,origin.x1+1,a.y2};rect.bg_color=lv_color_hex(0x926cc9);rect.radius=0;lv_draw_rect(layer,&rect,&bar);}
        if(!before && (d->style&MUSE_RICH_STRIKE)){lv_area_t line={a.x1,a.y1+8,a.x2,a.y1+8};rect.bg_color=lv_color_hex(0xe1d3f5);lv_draw_rect(layer,&rect,&line);}
        if(!before && (d->style&MUSE_RICH_LINK)){lv_area_t line={a.x1,a.y2,a.x2,a.y2};rect.bg_color=lv_color_hex(0xbd94ff);lv_draw_rect(layer,&rect,&line);}
    }
}
static void reader_link_close(lv_event_t *e){lv_obj_delete(lv_obj_get_parent(lv_event_get_target(e)));}
static void reader_link(lv_event_t *e)
{
    (void)e;if(!s_indev || !s_rich_meta || !s_rich_meta->link_count)return;
    s_reader_phase=30;
    lv_point_t point;lv_indev_get_point(s_indev,&point);lv_area_t area;lv_obj_get_coords(s_conversation_label,&area);point.x-=area.x1;point.y-=area.y1;point.y+=s_reader_window_top;
    uint32_t letter=reader_letter_on(&point);
    for(unsigned i=0;i<s_rich_meta->link_count;i++){
        muse_rich_link_t *link=&s_rich_meta->links[i];if(letter<link->start || letter>=link->end)continue;
        lv_obj_t *box=lv_obj_create(lv_layer_top());lv_obj_set_size(box,300,200);lv_obj_center(box);lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(box,lv_color_hex(0x170e22),0);lv_obj_set_style_border_color(box,lv_color_hex(0x9c6aff),0);lv_obj_set_style_radius(box,12,0);
        lv_obj_t *title=make_label(box,&lv_font_montserrat_16,0xd4bdff);lv_label_set_text(title,"Link address");lv_obj_align(title,LV_ALIGN_TOP_MID,0,-4);
        lv_obj_t *body=lv_obj_create(box);lv_obj_remove_style_all(body);lv_obj_set_pos(body,0,27);lv_obj_set_size(body,268,108);lv_obj_set_scroll_dir(body,LV_DIR_VER);
        lv_obj_t *url=make_label(body,&lv_font_montserrat_14,0xf3edff);lv_obj_set_width(url,264);lv_label_set_long_mode(url,LV_LABEL_LONG_MODE_WRAP);lv_label_set_text(url,link->url);
        lv_obj_t *close=lv_button_create(box);lv_obj_set_size(close,110,32);lv_obj_align(close,LV_ALIGN_BOTTOM_MID,0,0);lv_obj_add_event_cb(close,reader_link_close,LV_EVENT_CLICKED,NULL);
        lv_obj_t *label=make_label(close,&lv_font_montserrat_14,0xffffff);lv_label_set_text(label,"Back");lv_obj_center(label);break;
    }
}

static void conversation_latest(void)
{
    if (!s_conversation_body) return;
    s_conversation_scrolling = true;
    lv_obj_update_layout(s_conversation_body);
    lv_point_t pos;
    reader_letter_pos(s_conversation_latest_char, &pos);
    s_conversation_latest_y = pos.y;
    lv_obj_scroll_to_y(s_conversation_body, s_conversation_latest_y, LV_ANIM_OFF);
    s_conversation_scrolling = false;
}

static void conversation_size(void)
{
    lv_point_t anchor_point = {.x = 0, .y = lv_obj_get_scroll_y(s_conversation_body)};
    uint32_t anchor = reader_letter_on(&anchor_point);
    s_reader_decor_dirty=true;s_reader_layout_dirty=true;s_reader_layout_valid=false;
    unsigned mode = s_conversation_size_mode;
    int w = s_w - (mode == 2 ? 8 : 16);
    int loading=s_loading_visible?38:0;
    int h = mode == 2 ? s_h - 48 - (s_transport_visible ? MUSE_TRANSPORT_RESERVED_PX : 0) : (mode == 1 ? 94 : 74)+loading;
    lv_obj_set_pos(s_conversation, mode == 2 ? 4 : 8, mode == 2 ? 46 : s_h - (s_transport_visible ? MUSE_TRANSPORT_RESERVED_PX : 30) - h);
    lv_obj_set_size(s_conversation, w, h);
    int transport=0; /* the container already reserves the playback row */
    lv_obj_set_y(s_conversation_body,30+loading);
    lv_obj_set_size(s_conversation_body, w - 18, h - 38 - (mode == 2 ? 28 : 0) - transport-loading);
    if(s_loading_panel){lv_obj_set_width(s_loading_panel,w-18);lv_obj_set_width(s_loading_bar,w-18);lv_obj_set_width(s_loading_text,w-24);lv_obj_set_width(s_loading_note,w-18);}
    if(s_archive_footer){
        lv_obj_set_pos(s_archive_footer,7,h-30-transport);lv_obj_set_width(s_archive_footer,w-14);
        if(mode==2)lv_obj_remove_flag(s_archive_footer,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_archive_footer,LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(s_conversation_label, w - 28);
    lv_obj_set_width(s_conversation_header, w - 142);
    lv_obj_set_x(s_conversation_follow_button, w - 142);
    lv_obj_set_x(s_conversation_latest_button, w - 66);
    lv_obj_set_style_bg_opa(s_conversation, mode == 2 ? LV_OPA_90 : LV_OPA_80, 0);
    lv_label_set_text(s_conversation_title, mode == 2 ? "Conversation / Max" : mode == 1 ? "Conversation / Card" : "Conversation / 2");
    lv_obj_update_layout(s_conversation_body);
    if (!s_conversation_follow) {
        reader_letter_pos(anchor, &anchor_point);
        s_conversation_scrolling = true;
        lv_obj_scroll_to_y(s_conversation_body, anchor_point.y, LV_ANIM_OFF);
        s_conversation_scrolling = false;
    }
    s_conversation_word_seen = UINT32_MAX;
}

#if LV_USE_SNAPSHOT
static char s_library_test_chat[33],s_library_test_allowed[33];
static volatile int s_library_test_action=-1;
static bool s_library_narration_test;
static uint32_t s_library_narration_generation;
void muse_ui_bench_library_test(const char *chat,int action)
{
    if(!chat || strlen(chat)!=32)return;
    strlcpy(s_library_test_chat,chat,33);s_library_test_action=action;
}
static void library_test_tick(void)
{
    if(s_library_narration_test){
        muse_module_llm_playback_t state=muse_module_llm_playback(s_library_narration_generation);
        if(state==MUSE_MODULE_LLM_PLAYBACK_DONE || state==MUSE_MODULE_LLM_PLAYBACK_FAILED || state==MUSE_MODULE_LLM_PLAYBACK_CANCELLED)s_library_narration_test=false;
    }
    if(s_library_test_action<0)return;
    int action=s_library_test_action;s_library_test_action=-1;
    if(action==1){
        /* A validated synthetic fixture remains eligible after its title changes. */
        for(unsigned i=0;i<s_archive_view.count;i++)if(!strcmp(s_library_test_chat,s_archive_view.items[i].id) && (!strncmp(s_archive_view.items[i].title,"[Temporary library test]",24) || !strcmp(s_library_test_allowed,s_library_test_chat))){
            strlcpy(s_library_test_allowed,s_library_test_chat,33);strlcpy(s_library_chat,s_library_test_chat,33);strlcpy(s_library_name,s_archive_view.items[i].title,sizeof(s_library_name));library_dialog(1);break;
        }
    }else if(action==27 && !strcmp(s_library_test_chat,s_library_test_allowed) && !strcmp(s_archive_view.chat,s_library_test_allowed) && muse_state_mode(NULL)==MUSE_MODE_IDLE){
        muse_module_llm_status_t voice;muse_module_llm_status(&voice);
        if(voice.state==MUSE_MODULE_LLM_READY){
            char *text=heap_caps_calloc(1,3200,MUSE_BIG_CAPS);if(!text)return;
            const char *paragraph="The hallway was **dark**, but the blue lantern glowed. SCP-9192 paid $12, then $45.12, beside the silver gate in ZIP 00501, listening to the rain. The keeper smiled and said, \"Let us go home.\" Then they walked together, past the quiet river, into the warm light.\n\n";
            for(unsigned i=0;i<9;i++)strlcat(text,paragraph,3200);
            strlcat(text,"**The End**",3200);
            library_close();s_archive_visible=false;s_conversation_preview=false;s_conversation_size_mode=2;s_conversation_follow=true;
            for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
            lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
            muse_conversation_clear();uint32_t id=muse_conversation_add(false,text);muse_conversation_bind_speech(id);s_conversation_version=UINT32_MAX;conversation_size();
            s_library_narration_generation=esp_random();s_library_narration_test=muse_module_llm_speak(s_library_narration_generation,text,muse_settings_volume())==ESP_OK;
            printf("@narration_test {\"queued\":%s,\"characters\":%u}\n",s_library_narration_test?"true":"false",(unsigned)strlen(text));fflush(stdout);free(text);
        }
    }else if(action!=0 && !strcmp(s_library_test_chat,s_library_test_allowed) && !strcmp(s_library_chat,s_library_test_allowed) && s_library_dialog)library_action(action);
    printf("@library_ui {\"mode\":%d,\"waiting\":%s,\"has_title_token\":%s}\n",s_library_mode,s_library_waiting?"true":"false",s_library_token[0]?"true":"false");fflush(stdout);
}
#endif

#if CONFIG_MUSE_MODULE_LLM
#include "muse_status_model.h"
static lv_obj_t *s_status_header,*s_status_battery,*s_status_fill,*s_status_text,*s_status_fill_text,*s_status_charge,*s_status_bars[4],*s_status_quota,*s_status_quota_text,*s_status_dialog,*s_status_wifi_percent,*s_status_volume_icon,*s_status_volume_text;
static lv_obj_t *s_pause_button,*s_pause_label,*s_settings_shortcut,*s_stop_button,*s_restart_button;
static int s_preview_volume=-1;
static uint32_t s_touch_presses;
void muse_ui_preview_volume(int pct){s_preview_volume=pct>=0 && pct<=100?pct:-1;}
#if LV_USE_SNAPSHOT
static volatile int s_playback_test_action=-1;
static int s_playback_test_volume=-1;
void muse_ui_bench_playback(int action){if(action>=0 && action<=33)s_playback_test_action=action;}
#endif
static void playback_pause_clicked(lv_event_t *e){
    (void)e;muse_state_poke();muse_module_llm_status_t st;muse_module_llm_status(&st);
    /* Ignore repeated taps until the audio owner acknowledges the transition. */
    if((st.state==MUSE_MODULE_LLM_SPEAKING||st.state==MUSE_MODULE_LLM_PREPARING) && st.pause_requested==st.paused)muse_module_llm_toggle_pause();
}
static void playback_stop_clicked(lv_event_t *e){(void)e;muse_state_poke();muse_chat_playback_control(false);}
static void playback_restart_clicked(lv_event_t *e){(void)e;muse_state_poke();muse_chat_playback_control(true);}
static lv_obj_t *transport_button(int x,int width,const char *text,lv_event_cb_t callback,bool primary){
    lv_obj_t *b=lv_button_create(lv_screen_active());lv_obj_remove_style_all(b);
    lv_obj_set_pos(b,x,s_h-31);lv_obj_set_size(b,width,29);lv_obj_set_style_radius(b,7,0);
    lv_obj_set_style_pad_all(b,0,0);lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(primary?0x4d3065:0x322042),0);
    lv_obj_set_style_border_color(b,lv_color_hex(primary?0x9f7bcf:0x8e68b6),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(primary?0x624080:0x472e56),LV_STATE_PRESSED);
    lv_obj_set_style_opa(b,LV_OPA_50,LV_STATE_DISABLED);
    lv_obj_t *l=make_label(b,&lv_font_montserrat_14,0xeee5ff);lv_label_set_text(l,text);lv_obj_center(l);
    lv_obj_set_ext_click_area(b,1);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b,callback,LV_EVENT_CLICKED,NULL);lv_obj_add_flag(b,LV_OBJ_FLAG_HIDDEN);return b;
}
static void playback_sound_clicked(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    if(!muse_settings_speaker_on()) {
        muse_settings_set_speaker_on(true);
        muse_module_llm_set_playback_volume(muse_settings_volume());
        return;
    }
    if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);
    muse_settings_ui_open_sound();
}
static void playback_settings_clicked(lv_event_t *e){(void)e;muse_state_poke();if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_home();}
static void status_popup(lv_event_t *e);
static void usage_check_clicked(lv_event_t *e);
static void status_popup_close(lv_event_t *e);
static void playback_usage_popup(lv_event_t *e){(void)e;status_popup(e);}
static void playback_button_build(void)
{
    if(s_w<300)return;
    const int gap=12,width=(s_w-16-2*gap)/3;
    int x=(s_w-(3*width+2*gap))/2;
    s_stop_button=transport_button(x,width,LV_SYMBOL_STOP " Stop",playback_stop_clicked,false);
    s_pause_button=transport_button(x+width+gap,width,LV_SYMBOL_PAUSE " Pause",playback_pause_clicked,true);s_pause_label=lv_obj_get_child(s_pause_button,0);
    s_restart_button=transport_button(x+2*(width+gap),width,LV_SYMBOL_REFRESH " Restart",playback_restart_clicked,false);
    s_settings_shortcut=lv_button_create(lv_screen_active());lv_obj_set_pos(s_settings_shortcut,s_w-30,1);lv_obj_set_size(s_settings_shortcut,28,20);
    lv_obj_set_style_radius(s_settings_shortcut,5,0);lv_obj_set_style_pad_all(s_settings_shortcut,0,0);lv_obj_set_style_bg_color(s_settings_shortcut,lv_color_hex(0x221532),0);lv_obj_set_style_border_color(s_settings_shortcut,lv_color_hex(0x69488f),0);lv_obj_set_style_border_width(s_settings_shortcut,1,0);
    lv_obj_t *gear=make_label(s_settings_shortcut,&lv_font_montserrat_14,0xd4bdff);lv_label_set_text(gear,LV_SYMBOL_SETTINGS);lv_obj_center(gear);
    lv_obj_set_ext_click_area(s_settings_shortcut,3);lv_obj_add_event_cb(s_settings_shortcut,playback_settings_clicked,LV_EVENT_CLICKED,NULL);
}
static void playback_button_tick(void)
{
    muse_module_llm_set_playback_volume(muse_settings_speaker_on()?(s_preview_volume>=0?s_preview_volume:muse_settings_volume()):0);
    if(!s_pause_button)return;
    muse_module_llm_status_t st;muse_module_llm_status(&st);
    bool active=muse_chat_playback_active() || st.state==MUSE_MODULE_LLM_PREPARING || st.state==MUSE_MODULE_LLM_SPEAKING || st.state==MUSE_MODULE_LLM_CANCELLING;
    bool visible=!muse_state_asleep() && !muse_memory_recorder_active() && (active || (muse_chat_playback_can_restart()&&st.state==MUSE_MODULE_LLM_READY));
    lv_obj_set_flag(s_pause_button,LV_OBJ_FLAG_HIDDEN,!visible);lv_obj_set_flag(s_stop_button,LV_OBJ_FLAG_HIDDEN,!visible);lv_obj_set_flag(s_restart_button,LV_OBJ_FLAG_HIDDEN,!visible);
    bool in_settings=s_tv && lv_tileview_get_tile_active(s_tv)==s_settings;
    if(visible!=s_transport_visible){s_transport_visible=visible;if(s_conversation)conversation_size();}
    muse_settings_ui_transport_visible(visible&&in_settings);
    lv_obj_set_flag(s_settings_shortcut,LV_OBJ_FLAG_HIDDEN,in_settings || muse_state_asleep());
    bool pending=st.pause_requested!=st.paused;
    bool enabled=visible && (st.state==MUSE_MODULE_LLM_SPEAKING||st.state==MUSE_MODULE_LLM_PREPARING) && !pending && !muse_chat_playback_busy();
    lv_obj_set_state(s_pause_button,LV_STATE_DISABLED,!enabled);
    lv_obj_set_state(s_stop_button,LV_STATE_DISABLED,!active || st.state==MUSE_MODULE_LLM_CANCELLING || muse_chat_playback_busy());
    lv_obj_set_state(s_restart_button,LV_STATE_DISABLED,!muse_chat_playback_can_restart() || muse_chat_playback_busy() || !(st.state==MUSE_MODULE_LLM_READY || st.state==MUSE_MODULE_LLM_SPEAKING || st.state==MUSE_MODULE_LLM_PREPARING));
    const char *label=st.paused?LV_SYMBOL_PLAY " Resume":LV_SYMBOL_PAUSE " Pause";
    if(strcmp(label,lv_label_get_text(s_pause_label)))lv_label_set_text(s_pause_label,label);

#if LV_USE_SNAPSHOT
    if(s_playback_test_action>=0){
        int action=s_playback_test_action;s_playback_test_action=-1;
        if(action==28)s_conversation_version=UINT32_MAX;
        if(action==29){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_commands();}
        if(action==30){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_startup();}
        if(action==31)muse_startup_show(true);
        if(action==32||action==33)muse_settings_ui_bench_startup(action==33);
        if(action==26)playback_stop_clicked(NULL);
        if(action==27)playback_restart_clicked(NULL);
        if(action==1 && !lv_obj_has_state(s_pause_button,LV_STATE_DISABLED))lv_obj_send_event(s_pause_button,LV_EVENT_CLICKED,NULL);
        if(action==9){lv_obj_t *b=lv_button_create(lv_screen_active());lv_obj_add_event_cb(b,playback_usage_popup,LV_EVENT_CLICKED,(void *)1);lv_obj_send_event(b,LV_EVENT_CLICKED,NULL);lv_obj_delete(b);}
        if(action==10)usage_check_clicked(NULL);
        if(action==11)status_popup_close(NULL);
        if(action==8 && !lv_obj_has_flag(s_settings_shortcut,LV_OBJ_FLAG_HIDDEN))lv_obj_send_event(s_settings_shortcut,LV_EVENT_CLICKED,NULL);
        if(action==12){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_weather();}
        if(action==13)muse_ui_navigation(-1);
        if(action==14)muse_ui_navigation(1);
        if(action==15)muse_settings_ui_bench_weather_forecast();
        if(action==16){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_recorder();}
        if(action==17)muse_memory_recorder_action("recstart",1,100);
        if(action==18)muse_memory_recorder_action("recstop",0,0);
        if(action==19)muse_ui_recorder_wake();
        if(action>=23 && action<=25){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_bench_recorder_sleep(action==24?-1:action==25?1:0);printf("@recorder_sleep {\"enforced\":%s,\"held\":%s,\"asleep\":%s}\n",muse_settings_recorder_sleep_enforced()?"true":"false",muse_settings_ui_recorder_keep_awake()?"true":"false",muse_state_asleep()?"true":"false");fflush(stdout);}
        if(action==22){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_bench_recorder_status();}
        if(action==20||action==21){if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_bench_recorder_reader(action==21);}
        if(action==2)playback_sound_clicked(NULL);
        if(action==3 && s_conversation_title)lv_obj_send_event(lv_obj_get_parent(s_conversation_title),LV_EVENT_CLICKED,NULL);
        if(action==4 && s_tv)lv_tileview_set_tile(s_tv,s_face,LV_ANIM_OFF);
        if(action==5 && s_library_narration_test)muse_module_llm_cancel(s_library_narration_generation);
        if(action==6 && (st.state==MUSE_MODULE_LLM_SPEAKING || st.state==MUSE_MODULE_LLM_PREPARING)){if(s_playback_test_volume<0)s_playback_test_volume=muse_settings_volume();muse_settings_ui_bench_volume(s_playback_test_volume/2,false);}
        if(action==7 && s_playback_test_volume>=0){muse_settings_ui_bench_volume(s_playback_test_volume,true);s_playback_test_volume=-1;}
        muse_module_llm_status(&st);
        printf("@startup {\"enabled\":%s,\"visible\":%s,\"version\":\"%s\"}\n",muse_settings_startup_splash()?"true":"false",muse_startup_visible()?"true":"false",MUSE_STORYTELLER_VERSION);
        printf("@loading_ui {\"visible\":%s,\"remaining_percent\":%u,\"seconds\":%u,\"late\":%s}\n",s_loading_visible?"true":"false",s_loading_view.percent,s_loading_view.seconds,s_loading_view.late?"true":"false");
        printf("@playback_ui {\"speaker\":%s,\"restart_available\":%s,\"transport_busy\":%s,\"brightness\":%d,\"speed\":%d,\"paused\":%s,\"requested\":%s,\"visible\":%s,\"reader_size\":%d,\"settings\":%s,\"subpage\":%s,\"volume\":%d,\"preview_volume\":%d,\"touch_presses\":%lu}\n",muse_settings_speaker_on()?"true":"false",muse_chat_playback_can_restart()?"true":"false",muse_chat_playback_busy()?"true":"false",muse_settings_brightness(),muse_settings_speech_speed(),st.paused?"true":"false",st.pause_requested?"true":"false",visible?"true":"false",s_conversation_size_mode,s_tv && lv_tileview_get_tile_active(s_tv)==s_settings?"true":"false",muse_settings_ui_in_subpage()?"true":"false",muse_settings_volume(),s_preview_volume,(unsigned long)s_touch_presses);fflush(stdout);
        uint32_t model_version=0;unsigned model_count=0;size_t model_bytes=0;muse_conversation_diagnostic(&model_version,&model_count,&model_bytes);
        printf("@conversation_ui {\"version\":%lu,\"count\":%u,\"bytes\":%u,\"shown_version\":%lu,\"rendered_chars\":%u,\"touching\":%s,\"preview\":%s,\"archive\":%s}\n",(unsigned long)model_version,model_count,(unsigned)model_bytes,(unsigned long)s_conversation_version,(unsigned)s_reader_point_count,s_indev&&lv_indev_get_state(s_indev)==LV_INDEV_STATE_PRESSED?"true":"false",s_conversation_preview?"true":"false",s_archive_visible?"true":"false");fflush(stdout);
    }
#endif
}
static portMUX_TYPE s_usage_lock=portMUX_INITIALIZER_UNLOCKED;
static int s_usage_remaining=-1,s_usage_reset=-1;
static uint32_t s_usage_received,s_usage_valid_ms;
#if LV_USE_SNAPSHOT
static volatile int s_status_demo_pct=-2,s_status_demo_rssi=-52,s_status_demo_charge,s_status_demo_quota=-1;
void muse_ui_bench_status(int pct,int charging,int rssi,int quota)
{
    s_status_demo_charge=charging;s_status_demo_rssi=rssi;s_status_demo_quota=quota;s_status_demo_pct=pct;
}
#endif
void muse_ui_usage_update(int remaining_pct,int reset_after_s,unsigned valid_for_ms)
{
    portENTER_CRITICAL(&s_usage_lock);
    s_usage_remaining=muse_status_percent_valid(remaining_pct)?remaining_pct:-1;s_usage_reset=reset_after_s>=0?reset_after_s:-1;
    s_usage_received=(uint32_t)(esp_timer_get_time()/1000);s_usage_valid_ms=valid_for_ms>600000?600000:valid_for_ms;
    portEXIT_CRITICAL(&s_usage_lock);
}
static lv_obj_t *s_usage_popup_body,*s_usage_check_button,*s_usage_check_label;
static lv_obj_t *s_usage_value,*s_usage_meter,*s_usage_reading,*s_usage_synced,*s_usage_result;
static bool s_usage_popup_open,s_usage_local_pending,s_usage_failed;
static uint32_t s_usage_request_at,s_usage_wait_at;
static bool s_usage_waiting;
static char s_usage_before_id[33];
static uint32_t s_usage_popup_at,s_usage_popup_query;
static void usage_popup_tick(void)
{
    if(!s_usage_popup_open || !s_usage_popup_body)return;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    if((int32_t)(now-s_usage_popup_at)<250)return;
    s_usage_popup_at=now;
    muse_usage_view_t account={0};bool valid=muse_memory_usage_copy(&account);
    if(s_usage_local_pending && account.refresh_id[0] && strcmp(account.refresh_id,s_usage_before_id))s_usage_local_pending=false;
    if(s_usage_local_pending && now-s_usage_request_at>180000){s_usage_local_pending=false;s_usage_failed=true;}
    if(account.refresh_pending){if(!s_usage_waiting){s_usage_wait_at=now;s_usage_waiting=true;}if(now-s_usage_wait_at>180000)s_usage_failed=true;}else s_usage_waiting=false;
    if(s_usage_failed && !strcmp(account.refresh_state,"done") && account.refresh_id[0] && strcmp(account.refresh_id,s_usage_before_id))s_usage_failed=false;
    bool pending=(account.refresh_pending || s_usage_local_pending) && !s_usage_failed;
    if(now-s_usage_popup_query>(pending?2000:10000)){s_usage_popup_query=now;muse_memory_usage_poll();}
    char text[160];
    if(valid)snprintf(text,sizeof(text),"%u%% left*",100-account.used);else strlcpy(text,"No reading yet",sizeof(text));
    if(strcmp(text,lv_label_get_text(s_usage_value)))lv_label_set_text(s_usage_value,text);
    lv_bar_set_value(s_usage_meter,valid?100-account.used:0,LV_ANIM_OFF);
    if(valid)snprintf(text,sizeof(text),"Website: %u%% used  /  Resets %s",account.observed,account.reset);else strlcpy(text,"Check Muse to get your first reading",sizeof(text));
    if(strcmp(text,lv_label_get_text(s_usage_reading)))lv_label_set_text(s_usage_reading,text);
    snprintf(text,sizeof(text),"Last sync: %s",account.last_sync[0]?account.last_sync:valid?account.checked:"Never");
    if(strcmp(text,lv_label_get_text(s_usage_synced)))lv_label_set_text(s_usage_synced,text);
    const char *result="Check via your PC";unsigned color=0xc1ade3;
    if(s_usage_failed){result="Not updated - saved reading kept";color=0xffab48;}
    else if(s_usage_local_pending)result="Sending request...";
    else if(!strcmp(account.refresh_state,"queued"))result="Waiting for PC + Chrome...";
    else if(!strcmp(account.refresh_state,"checking"))result="Reading Muse in Chrome...";
    else if(s_usage_failed || !strcmp(account.refresh_state,"error")){result="Not updated - saved reading kept";color=0xffab48;}
    else if(!strcmp(account.refresh_state,"done")){result=LV_SYMBOL_OK " Synced successfully";color=0x5ff0b9;}
    if(strcmp(result,lv_label_get_text(s_usage_result)))lv_label_set_text(s_usage_result,result);
    lv_obj_set_style_text_color(s_usage_result,lv_color_hex(color),0);
    const char *help="*Estimate works without PC. Live sync:\nPC + signed-in Chrome + Module USB.";
    if(s_usage_failed)help="Module did not reply. Check its power\nand USB, then retry. Saved reading kept.";
    else if(!strcmp(account.refresh_state,"error")){
        if(strstr(account.refresh_note,"Sign in"))help="Sign in to Muse in PC Chrome,\nthen retry. Saved reading kept.";
        else if(strstr(account.refresh_note,"card"))help="Could not read Muse's usage page.\nOpen Muse in PC Chrome, then retry.";
        else if(strstr(account.refresh_note,"sync failed"))help="Website read; device sync failed.\nCheck Module USB, then retry.";
        else help="Connect Module USB and open Chrome\non your PC, then retry.";
    }
    if(strcmp(help,lv_label_get_text(s_usage_popup_body)))lv_label_set_text(s_usage_popup_body,help);
    const char *name=pending?"Checking...":s_usage_failed||!strcmp(account.refresh_state,"error")?"Retry via PC":"Sync via PC";
    if(strcmp(name,lv_label_get_text(s_usage_check_label)))lv_label_set_text(s_usage_check_label,name);
    if(account.refresh_supported && !pending)lv_obj_remove_state(s_usage_check_button,LV_STATE_DISABLED);else lv_obj_add_state(s_usage_check_button,LV_STATE_DISABLED);
}
static void usage_check_clicked(lv_event_t *e)
{
    (void)e;muse_state_poke();
    muse_usage_view_t before={0};muse_memory_usage_copy(&before);
    if(!s_usage_check_button || s_usage_local_pending || (before.refresh_pending&&!s_usage_failed))return;
    if(muse_memory_usage_check()){
        s_usage_failed=false;s_usage_waiting=true;s_usage_wait_at=(uint32_t)(esp_timer_get_time()/1000);s_usage_local_pending=true;s_usage_request_at=(uint32_t)(esp_timer_get_time()/1000);strlcpy(s_usage_before_id,before.refresh_id,sizeof(s_usage_before_id));
        s_usage_popup_at=0;usage_popup_tick();
    }else{s_usage_failed=true;s_usage_popup_at=0;usage_popup_tick();}
}
static void status_popup_close(lv_event_t *e)
{
    (void)e;s_usage_popup_open=false;s_usage_popup_body=NULL;s_usage_check_button=NULL;s_usage_check_label=NULL;s_usage_value=NULL;s_usage_meter=NULL;s_usage_result=NULL;if(s_status_dialog)lv_obj_delete(s_status_dialog);s_status_dialog=NULL;
}
static void status_popup(lv_event_t *e)
{
    muse_state_poke();status_popup_close(NULL);
    bool quota=(intptr_t)lv_event_get_user_data(e)==1;
    s_status_dialog=lv_obj_create(lv_layer_top());lv_obj_set_size(s_status_dialog,300,quota?226:198);lv_obj_center(s_status_dialog);lv_obj_remove_flag(s_status_dialog,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_status_dialog,lv_color_hex(0x191028),0);lv_obj_set_style_border_color(s_status_dialog,lv_color_hex(0x9c6aff),0);lv_obj_set_style_radius(s_status_dialog,14,0);
    if(quota){lv_obj_set_style_pad_all(s_status_dialog,10,0);lv_obj_set_style_border_width(s_status_dialog,1,0);}
    lv_obj_t *title=make_label(s_status_dialog,&lv_font_montserrat_16,0xd4bdff);lv_label_set_text(title,quota?"Muse account usage":"Live device status");lv_obj_align(title,LV_ALIGN_TOP_MID,0,0);
    lv_obj_t *body=make_label(s_status_dialog,&lv_font_montserrat_14,0xf3edff);lv_obj_set_pos(body,0,30);lv_obj_set_width(body,266);
    if(quota){
        lv_obj_delete(body);
        s_usage_value=make_label(s_status_dialog,&lv_font_montserrat_28,0xf3edff);lv_obj_set_pos(s_usage_value,0,23);lv_obj_set_width(s_usage_value,278);lv_obj_set_style_text_align(s_usage_value,LV_TEXT_ALIGN_CENTER,0);
        s_usage_meter=lv_bar_create(s_status_dialog);lv_obj_set_pos(s_usage_meter,0,57);lv_obj_set_size(s_usage_meter,278,7);lv_bar_set_range(s_usage_meter,0,100);lv_obj_set_style_bg_color(s_usage_meter,lv_color_hex(0x332445),0);lv_obj_set_style_bg_color(s_usage_meter,lv_color_hex(0x9c6aff),LV_PART_INDICATOR);
        s_usage_reading=make_label(s_status_dialog,&lv_font_montserrat_14,0xd4bdff);lv_obj_set_pos(s_usage_reading,0,72);lv_obj_set_width(s_usage_reading,278);lv_obj_set_style_text_align(s_usage_reading,LV_TEXT_ALIGN_CENTER,0);
        s_usage_synced=make_label(s_status_dialog,&lv_font_montserrat_14,0xbcb0cf);lv_obj_set_pos(s_usage_synced,0,90);lv_obj_set_width(s_usage_synced,278);lv_obj_set_style_text_align(s_usage_synced,LV_TEXT_ALIGN_CENTER,0);
        s_usage_result=make_label(s_status_dialog,&lv_font_montserrat_14,0x5ff0b9);lv_obj_set_pos(s_usage_result,0,112);lv_obj_set_width(s_usage_result,278);lv_obj_set_style_text_align(s_usage_result,LV_TEXT_ALIGN_CENTER,0);
        s_usage_popup_body=make_label(s_status_dialog,&lv_font_montserrat_14,0xbcb0cf);lv_obj_set_pos(s_usage_popup_body,0,137);lv_obj_set_width(s_usage_popup_body,278);lv_obj_set_style_text_align(s_usage_popup_body,LV_TEXT_ALIGN_CENTER,0);
        s_usage_popup_open=true;s_usage_popup_at=0;s_usage_popup_query=0;
        s_usage_check_button=lv_button_create(s_status_dialog);lv_obj_set_size(s_usage_check_button,180,32);lv_obj_align(s_usage_check_button,LV_ALIGN_BOTTOM_LEFT,0,0);lv_obj_set_style_bg_color(s_usage_check_button,lv_color_hex(0x623ca0),0);lv_obj_set_style_radius(s_usage_check_button,10,0);
        lv_obj_add_event_cb(s_usage_check_button,usage_check_clicked,LV_EVENT_CLICKED,NULL);
        s_usage_check_label=make_label(s_usage_check_button,&lv_font_montserrat_14,0xffffff);lv_label_set_text(s_usage_check_label,"Sync via PC");lv_obj_center(s_usage_check_label);
        usage_popup_tick();

    }else{
        muse_wifi_status_t w;muse_wifi_status(&w);muse_power_t p=muse_state_power();char text[224],battery[48],signal[64];
        if(muse_status_percent_valid(p.battery_pct))snprintf(battery,sizeof(battery),"Core2 battery: %d%% / %s",p.battery_pct,p.charging?"charging":p.usb?"USB power":"battery power");
        else snprintf(battery,sizeof(battery),"Core2 battery: unavailable");
        if(w.state==MUSE_WIFI_CONNECTED && w.rssi<0)snprintf(signal,sizeof(signal),"Wi-Fi: %d%% signal / %d dBm",muse_status_wifi_percent(true,w.rssi),w.rssi);
        else snprintf(signal,sizeof(signal),"Wi-Fi: %s",w.state==MUSE_WIFI_CONNECTED?"strength unavailable":"not connected");
        snprintf(text,sizeof(text),"%s\n\n%s\n\nModule LLM has separate power.",battery,signal);lv_label_set_text(body,text);
    }
    lv_obj_t *button=lv_button_create(s_status_dialog);lv_obj_set_size(button,quota?88:100,quota?32:30);lv_obj_align(button,quota?LV_ALIGN_BOTTOM_RIGHT:LV_ALIGN_BOTTOM_MID,0,0);lv_obj_add_event_cb(button,status_popup_close,LV_EVENT_CLICKED,NULL);
    lv_obj_t *back=make_label(button,&lv_font_montserrat_14,0xf3edff);lv_label_set_text(back,"Back");lv_obj_center(back);
}
static lv_obj_t *status_rect(lv_obj_t *parent,int x,int y,int w,int h,unsigned color)
{
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);return o;
}
static void status_header_build(lv_obj_t *face)
{
    if(s_w<300 || s_h<220)return;
    s_status_header=status_rect(face,0,0,s_w,44,0x07040f);
    if(s_aux_icon){lv_obj_set_parent(s_aux_icon,s_status_header);lv_obj_set_align(s_aux_icon,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(s_aux_icon,5,3);}
    lv_obj_add_flag(s_wifi_icon,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_ble_icon,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_power_lbl,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_parent(s_state_lbl,s_status_header);lv_obj_set_align(s_state_lbl,LV_ALIGN_TOP_LEFT);lv_obj_set_style_text_font(s_state_lbl,&lv_font_unscii_8,0);lv_obj_set_style_text_letter_space(s_state_lbl,0,0);lv_obj_set_pos(s_state_lbl,40,22);lv_obj_set_size(s_state_lbl,s_w-80,10);lv_obj_set_style_text_align(s_state_lbl,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(s_state_lbl,LV_LABEL_LONG_MODE_DOTS);
    for(unsigned i=0;i<4;i++){s_status_bars[i]=status_rect(s_status_header,26+i*7,17-(5+3*i),5,5+3*i,0x2e213f);lv_obj_set_style_radius(s_status_bars[i],1,0);}
    lv_obj_t *wifi_hit=lv_button_create(s_status_header);lv_obj_remove_style_all(wifi_hit);lv_obj_set_pos(wifi_hit,25,0);lv_obj_set_size(wifi_hit,72,22);lv_obj_add_event_cb(wifi_hit,status_popup,LV_EVENT_CLICKED,NULL);
    s_status_wifi_percent=make_label(s_status_header,&lv_font_unscii_8,0xd4bdff);lv_obj_set_pos(s_status_wifi_percent,58,7);lv_obj_set_size(s_status_wifi_percent,32,12);lv_obj_set_style_text_align(s_status_wifi_percent,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_text(s_status_wifi_percent,"--");
    /* One real control owns both readouts, including the padding between them. */
    lv_obj_t *sound_hit=lv_button_create(s_status_header);lv_obj_remove_style_all(sound_hit);lv_obj_set_pos(sound_hit,(s_w-80)/2,0);lv_obj_set_size(sound_hit,80,22);lv_obj_remove_flag(sound_hit,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);lv_obj_set_ext_click_area(sound_hit,3);lv_obj_add_event_cb(sound_hit,playback_sound_clicked,LV_EVENT_CLICKED,NULL);
    s_status_volume_icon=make_label(sound_hit,&lv_font_montserrat_14,0xd4bdff);lv_obj_set_pos(s_status_volume_icon,6,3);lv_obj_set_size(s_status_volume_icon,16,18);lv_obj_remove_flag(s_status_volume_icon,LV_OBJ_FLAG_CLICKABLE);lv_label_set_text(s_status_volume_icon,LV_SYMBOL_VOLUME_MID);
    s_status_volume_text=make_label(sound_hit,&lv_font_unscii_8,0xf3edff);lv_obj_set_pos(s_status_volume_text,26,7);lv_obj_set_size(s_status_volume_text,48,12);lv_obj_remove_flag(s_status_volume_text,LV_OBJ_FLAG_CLICKABLE);lv_label_set_text(s_status_volume_text,"--");
    s_status_charge=make_label(s_status_header,&lv_font_montserrat_14,0xffcf81);lv_label_set_text(s_status_charge,LV_SYMBOL_CHARGE);lv_obj_set_pos(s_status_charge,s_w-103,3);lv_obj_add_flag(s_status_charge,LV_OBJ_FLAG_HIDDEN);
    s_status_battery=status_rect(s_status_header,s_w-89,2,52,18,0x170e22);lv_obj_set_style_radius(s_status_battery,4,0);lv_obj_set_style_border_width(s_status_battery,1,0);lv_obj_set_style_border_color(s_status_battery,lv_color_hex(0x9b89b6),0);
    status_rect(s_status_header,s_w-36,8,3,6,0x9b89b6);
    s_status_fill=status_rect(s_status_battery,0,0,1,16,0x6fe5b6);lv_obj_set_style_radius(s_status_fill,3,0);
    s_status_text=make_label(s_status_battery,&lv_font_unscii_8,0xf3edff);lv_obj_set_pos(s_status_text,0,4);lv_obj_set_width(s_status_text,50);lv_obj_set_style_text_align(s_status_text,LV_TEXT_ALIGN_CENTER,0);
    s_status_fill_text=make_label(s_status_fill,&lv_font_unscii_8,0x1f0d2f);lv_obj_set_pos(s_status_fill_text,0,4);lv_obj_set_width(s_status_fill_text,50);lv_obj_set_style_text_align(s_status_fill_text,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_move_to_index(s_status_fill,-1);
    lv_obj_add_flag(s_status_battery,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_ext_click_area(s_status_battery,3);lv_obj_add_event_cb(s_status_battery,status_popup,LV_EVENT_CLICKED,NULL);
    s_status_quota=lv_bar_create(s_status_header);lv_obj_remove_style_all(s_status_quota);lv_obj_set_pos(s_status_quota,40,42);lv_obj_set_size(s_status_quota,s_w-80,2);lv_bar_set_range(s_status_quota,0,100);
    lv_obj_set_style_bg_color(s_status_quota,lv_color_hex(0x2e213f),0);lv_obj_set_style_bg_opa(s_status_quota,LV_OPA_COVER,0);lv_obj_set_style_radius(s_status_quota,2,0);
    lv_obj_set_style_bg_color(s_status_quota,lv_color_hex(0xad7cf6),LV_PART_INDICATOR);lv_obj_set_style_bg_opa(s_status_quota,LV_OPA_TRANSP,LV_PART_INDICATOR);lv_obj_set_style_radius(s_status_quota,2,LV_PART_INDICATOR);
    s_status_quota_text=make_label(s_status_header,&lv_font_unscii_8,0xa89cbc);lv_obj_set_pos(s_status_quota_text,40,33);lv_obj_set_width(s_status_quota_text,s_w-80);lv_obj_set_style_text_align(s_status_quota_text,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(s_status_quota_text,"Muse usage: waiting for sync");
    lv_obj_t *quota_hit=lv_button_create(s_status_header);lv_obj_remove_style_all(quota_hit);lv_obj_set_pos(quota_hit,38,32);lv_obj_set_size(quota_hit,s_w-76,12);lv_obj_add_event_cb(quota_hit,status_popup,LV_EVENT_CLICKED,(void *)1);
}
static void status_header_update(const muse_wifi_status_t *wifi)
{
    if(!s_status_header)return;
    muse_power_t p=muse_state_power();int rssi=wifi->rssi,percent=p.battery_pct,quota,reset;bool connected=wifi->state==MUSE_WIFI_CONNECTED,charging=p.charging;
    uint32_t received,valid_ms;portENTER_CRITICAL(&s_usage_lock);quota=s_usage_remaining;reset=s_usage_reset;received=s_usage_received;valid_ms=s_usage_valid_ms;portEXIT_CRITICAL(&s_usage_lock);
    uint32_t elapsed=(uint32_t)(esp_timer_get_time()/1000)-received;
    if(elapsed>valid_ms || !valid_ms)quota=-1;
#if LV_USE_SNAPSHOT
    bool demo=s_status_demo_pct!=-2;
    if(demo){percent=s_status_demo_pct;rssi=s_status_demo_rssi;connected=rssi<0;charging=s_status_demo_charge;quota=s_status_demo_quota;reset=7200;elapsed=0;lv_label_set_text(s_state_lbl,"STATUS PREVIEW");}
#endif
    muse_usage_view_t account;bool calibrated=muse_memory_usage_copy(&account);
#if LV_USE_SNAPSHOT
    if(demo)calibrated=false;
#endif
    unsigned calibration_age=0;
    if(calibrated){quota=100-(int)account.used;calibration_age=account.age+((uint32_t)(esp_timer_get_time()/1000)-account.received_ms)/1000;}
    int bars=muse_status_wifi_bars(connected,rssi);
    for(unsigned i=0;i<4;i++)lv_obj_set_style_bg_color(s_status_bars[i],lv_color_hex((int)i<bars?0xb28aed:0x2e213f),0);
    char level[12];int quality=muse_status_wifi_percent(connected,rssi);
    if(quality<0)strlcpy(level,"--",sizeof(level));else snprintf(level,sizeof(level),"%d%%",quality);
    if(strcmp(level,lv_label_get_text(s_status_wifi_percent)))lv_label_set_text(s_status_wifi_percent,level);
    int volume=muse_settings_volume();bool muted=!muse_settings_speaker_on();
    if(muted)strlcpy(level,"MUTED",sizeof(level));else snprintf(level,sizeof(level),"%d%%",volume);
    if(strcmp(level,lv_label_get_text(s_status_volume_text)))lv_label_set_text(s_status_volume_text,level);
    lv_obj_set_style_text_color(s_status_volume_text,lv_color_hex(muted?0xffab48:0xf3edff),0);
    lv_obj_set_style_text_color(s_status_volume_icon,lv_color_hex(muted?0xffab48:0xd4bdff),0);
    const char *speaker=(muted||volume==0)?LV_SYMBOL_MUTE:volume<50?LV_SYMBOL_VOLUME_MID:LV_SYMBOL_VOLUME_MAX;
    if(strcmp(speaker,lv_label_get_text(s_status_volume_icon)))lv_label_set_text(s_status_volume_icon,speaker);
    char text[80];if(muse_status_percent_valid(percent))snprintf(text,sizeof(text),"%d%%",percent);else strlcpy(text,p.usb?"USB":"--",sizeof(text));
    if(strcmp(text,lv_label_get_text(s_status_text))){lv_label_set_text(s_status_text,text);lv_label_set_text(s_status_fill_text,text);}
    int fill=muse_status_fill(50,percent);lv_obj_set_width(s_status_fill,fill?fill:1);lv_obj_set_style_bg_opa(s_status_fill,fill?LV_OPA_COVER:LV_OPA_TRANSP,0);lv_obj_set_style_bg_color(s_status_fill,lv_color_hex(muse_status_battery_color(percent)),0);lv_obj_set_flag(s_status_fill_text,LV_OBJ_FLAG_HIDDEN,!fill);
    lv_obj_set_flag(s_status_charge,LV_OBJ_FLAG_HIDDEN,!charging);
    if(muse_status_percent_valid(quota)){
        int remaining=reset<0?-1:reset-(int)(elapsed/1000);if(remaining<0 && reset>=0)remaining=0;
        char when[36];if(remaining<0)strlcpy(when,"reset unavailable",sizeof(when));else if(remaining>=86400)snprintf(when,sizeof(when),"reset %dd %dh",remaining/86400,(remaining%86400)/3600);else if(remaining>=3600)snprintf(when,sizeof(when),"reset %dh %dm",remaining/3600,(remaining%3600)/60);else snprintf(when,sizeof(when),"reset %dm",remaining/60);
        snprintf(text,sizeof(text),"Muse %d%% left | %s",quota,when);lv_bar_set_value(s_status_quota,quota,LV_ANIM_OFF);lv_obj_set_style_bg_opa(s_status_quota,LV_OPA_COVER,LV_PART_INDICATOR);
    }else{strlcpy(text,"Muse usage: waiting for sync",sizeof(text));lv_bar_set_value(s_status_quota,0,LV_ANIM_OFF);lv_obj_set_style_bg_opa(s_status_quota,LV_OPA_TRANSP,LV_PART_INDICATOR);}
    if(calibrated){
        lv_bar_set_value(s_status_quota,(int)account.used,LV_ANIM_OFF);
        if(calibration_age>172800)snprintf(text,sizeof(text),"Muse last %u%%* | check needed",account.observed);
        else snprintf(text,sizeof(text),"Muse ~%u%% used* | reset %s",account.used,account.reset);
    }
    if(strcmp(text,lv_label_get_text(s_status_quota_text)))lv_label_set_text(s_status_quota_text,text);
}
#endif

static void conversation_click(lv_event_t *e)
{
    muse_state_poke();
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_archive_visible && (action == 1 || action == 2)) {
        s_archive_visible=false;
        lv_label_set_text(s_archive_save_label,"Save");
        for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
        lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
        s_conversation_version=UINT32_MAX;s_conversation_follow=true;
        conversation_size();return;
    }
    if (action == 2) {
        bool enabled = muse_settings_speech_follow();
        if (!enabled || !s_conversation_follow) {
            if (!enabled) muse_settings_set_speech_follow(true);
            s_conversation_follow = true;
            conversation_latest();
        } else {
            muse_settings_set_speech_follow(false);
        }
        s_conversation_word_seen = UINT32_MAX;
    } else if (action == 1) {
        s_conversation_follow = true;
        conversation_latest();
        s_conversation_word_seen = UINT32_MAX;
    } else {
        s_conversation_size_mode = (s_conversation_size_mode + 1) % 3;
        conversation_size();
    }
}

static void conversation_scroll(lv_event_t *e)
{
    (void)e;
    if (s_conversation_scrolling) return;
    if (s_indev && lv_indev_get_state(s_indev) == LV_INDEV_STATE_PRESSED) {
        muse_state_poke();
        s_conversation_follow = lv_obj_get_scroll_bottom(s_conversation_body) <= 12;
    }
}

static void archive_click(lv_event_t *e)
{
    muse_state_poke();
    int action=(int)(intptr_t)lv_event_get_user_data(e);
    if(action>=30 && action<35){
        unsigned i=action-30;if(i<s_archive_view.count){strlcpy(s_library_chat,s_archive_view.items[i].id,33);strlcpy(s_library_name,s_archive_view.items[i].title,sizeof(s_library_name));library_dialog(1);}
    } else if(action>=10 && action<15){
        unsigned i=action-10;if(i<s_archive_view.count)muse_memory_action("page",s_archive_view.items[i].id,0);
    } else if(action==3){muse_memory_action("list",NULL,0);}
    else if(action==4){muse_memory_action("new",NULL,0);}
    else if(action==2){
        if(s_archive_visible && !s_archive_view.library)muse_memory_action("resume",s_archive_view.chat,0);
        else muse_memory_action("save",NULL,0);
    } else if(s_archive_visible){
        if(s_archive_view.library){
            unsigned off=s_archive_view.library_offset;
            if(action==0 && off>=5)muse_memory_action("list",NULL,off-5);
            if(action==1 && s_archive_view.more)muse_memory_action("list",NULL,off+5);
        }else{
            unsigned page=s_archive_view.page;
            if(action==0 && page)muse_memory_action("page",s_archive_view.chat,page-1);
            if(action==1 && page+1<s_archive_view.pages)muse_memory_action("page",s_archive_view.chat,page+1);
        }
    }else if(action==0){muse_memory_action("page",NULL,0);}
}
static void archive_build(void)
{
    s_archive_text=heap_caps_calloc(1,8192,MUSE_BIG_CAPS);
    s_archive_footer=lv_obj_create(s_conversation);lv_obj_remove_style_all(s_archive_footer);
    lv_obj_remove_flag(s_archive_footer,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_archive_footer,lv_color_hex(0x140e20),0);
    lv_obj_set_style_bg_opa(s_archive_footer,LV_OPA_COVER,0);lv_obj_set_style_radius(s_archive_footer,7,0);
    const char *titles[]={"Older","Next","Save","Library","New"};
    const int widths[]={49,45,74,65,45};int x=0;
    for(unsigned i=0;i<5;i++){
        lv_obj_t *b=lv_button_create(s_archive_footer);lv_obj_remove_style_all(b);
        lv_obj_set_pos(b,x,1);lv_obj_set_size(b,widths[i],25);x+=widths[i]+3;
        lv_obj_set_style_radius(b,7,0);lv_obj_set_style_bg_color(b,lv_color_hex(0x9c6aff),0);
        lv_obj_set_style_bg_opa(b,LV_OPA_20,0);lv_obj_add_event_cb(b,archive_click,LV_EVENT_CLICKED,(void *)(intptr_t)i);
        lv_obj_t *label=make_label(b,&lv_font_montserrat_14,0xd4bdff);lv_label_set_text(label,titles[i]);lv_obj_center(label);
        if(i==2)s_archive_save_label=label;
    }
    for(unsigned i=0;i<5;i++){
        lv_obj_t *b=lv_button_create(s_conversation_body);s_archive_rows[i]=b;
        lv_obj_remove_style_all(b);lv_obj_set_pos(b,0,i*27);lv_obj_set_size(b,s_w-82,25);
        lv_obj_set_style_radius(b,6,0);lv_obj_set_style_bg_color(b,lv_color_hex(0x9c6aff),0);lv_obj_set_style_bg_opa(b,LV_OPA_10,0);
        lv_obj_add_event_cb(b,archive_click,LV_EVENT_CLICKED,(void *)(intptr_t)(10+i));
        lv_obj_t *label=make_label(b,&lv_font_montserrat_14,0xf3edff);lv_obj_set_width(label,s_w-94);
        lv_label_set_long_mode(label,LV_LABEL_LONG_MODE_DOTS);lv_obj_align(label,LV_ALIGN_LEFT_MID,5,0);
        lv_obj_add_flag(b,LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *more=lv_button_create(s_conversation_body);s_archive_more[i]=more;lv_obj_remove_style_all(more);
        lv_obj_set_pos(more,s_w-78,i*27);lv_obj_set_size(more,34,25);lv_obj_set_style_radius(more,6,0);
        lv_obj_set_style_bg_color(more,lv_color_hex(0x9c6aff),0);lv_obj_set_style_bg_opa(more,LV_OPA_20,0);
        lv_obj_add_event_cb(more,archive_click,LV_EVENT_CLICKED,(void *)(intptr_t)(30+i));
        lv_obj_t *dots=make_label(more,&lv_font_montserrat_14,0xf3edff);lv_label_set_text(dots,"...");lv_obj_center(dots);lv_obj_add_flag(more,LV_OBJ_FLAG_HIDDEN);
    }
}
static void archive_tick(void)
{
    uint32_t content=s_archive_view.content_version;
    if(!s_archive_text || !muse_memory_copy(&s_archive_view,s_archive_text,8192))return;
    if(s_library_dialog && s_library_waiting && s_archive_view.action_version!=s_library_action_seen && !strcmp(s_archive_view.action_chat,s_library_chat)){
        s_library_action_seen=s_archive_view.action_version;
        if(s_library_mode==3 && !strcmp(s_archive_view.action,"suggest")){
            s_library_waiting=false;strlcpy(s_library_token,s_archive_view.title_token,25);
            lv_label_set_text(s_library_title,s_archive_view.suggestion);lv_label_set_text(s_library_note,s_archive_view.title_choices>1?"Use this title, or try another.":"This is the clearest local title found.");lv_obj_remove_state(s_library_use,LV_STATE_DISABLED);
            if(s_archive_view.title_choices<2)lv_obj_add_state(s_library_another,LV_STATE_DISABLED);
        }else if(!strcmp(s_archive_view.action,"rename") || !strcmp(s_archive_view.action,"delete")){
            unsigned offset=s_archive_view.library_offset;library_close();muse_memory_action("list",NULL,offset);
        }
    }
    if(s_library_dialog && s_library_waiting && (strstr(s_archive_view.status,"could not") || strstr(s_archive_view.status,"unavailable") || strstr(s_archive_view.status,"Invalid"))){
        s_library_waiting=false;lv_label_set_text(s_library_note,"Couldn't finish. Please try again.");
    }
    if(content==s_archive_view.content_version){
        if(!s_archive_visible && strstr(s_archive_view.status,"bookmarked")){if(strcmp(lv_label_get_text(s_archive_save_label),"Saved"))lv_label_set_text(s_archive_save_label,"Saved");}
        if(strstr(s_archive_view.status,"Finish speaking")){if(strcmp(lv_label_get_text(s_conversation_title),"Finish reply first"))lv_label_set_text(s_conversation_title,"Finish reply first");}
        else if(strstr(s_archive_view.status,"unavailable") || strstr(s_archive_view.status,"could not") || strstr(s_archive_view.status,"not saved")){if(strcmp(lv_label_get_text(s_conversation_title),"Archive unavailable"))lv_label_set_text(s_conversation_title,"Archive unavailable");}
        return;
    }
    /* Ordinary draft ACKs update state, without replacing the live reader. */
    bool opened=s_archive_view.library || s_archive_view.pages;
    if(!opened)lv_label_set_text(s_archive_save_label,"Save");
    if(!opened && !s_archive_visible)return;
    if(strstr(s_archive_view.status,"Resumed") || strstr(s_archive_view.status,"New local")){
        s_archive_visible=false;s_archive_view.library=false;s_archive_view.pages=0;
        for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
        lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
        s_conversation_version=UINT32_MAX;s_conversation_follow=true;conversation_size();return;
    }
    s_archive_visible=true;s_conversation_size_mode=2;s_conversation_follow=false;
    conversation_size();
    reader_select_start(LV_LABEL_TEXT_SELECTION_OFF);
    reader_select_end(LV_LABEL_TEXT_SELECTION_OFF);
    if(s_archive_view.library){
        lv_label_set_text(s_conversation_title,"Local library");
        lv_obj_add_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_add_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
        for(unsigned i=0;i<5;i++){
            if(i<s_archive_view.count){
                archive_row_visible(i,true);
                lv_obj_t *label=lv_obj_get_child(s_archive_rows[i],0);
                lv_label_set_text_fmt(label,"%s%s",s_archive_view.items[i].saved?"* ":"",s_archive_view.items[i].title);
            }else archive_row_visible(i,false);
        }
        lv_label_set_text(s_archive_save_label,"Save");
    }else{
        lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
        for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
        conversation_render_style(s_archive_text[0]?s_archive_text:"This draft is empty.",s_archive_view.bold_start,s_archive_view.code_start,s_archive_view.style_start);
        lv_label_set_text_fmt(s_conversation_title,"Chat / %u of %u",s_archive_view.page+1,s_archive_view.pages);
        lv_label_set_text(s_archive_save_label,"Resume");
    }
    s_conversation_scrolling=true;lv_obj_scroll_to_y(s_conversation_body,0,LV_ANIM_OFF);s_conversation_scrolling=false;
}

static void conversation_build(lv_obj_t *face)
{
    if (!muse_board->touch || s_w < 300 || s_h < 220) return;
    s_conversation_text = heap_caps_calloc(1,MUSE_CONVERSATION_CAP, MUSE_BIG_CAPS);
    s_rich_text=heap_caps_malloc(MUSE_RICH_TEXT_CAP,MUSE_BIG_CAPS);
    s_rich_positions=heap_caps_calloc(MUSE_RICH_RAW_CAP+1,sizeof(uint16_t),MUSE_BIG_CAPS);
    if(!s_rich_text || !s_rich_positions){free(s_rich_text);free(s_rich_positions);s_rich_text=NULL;s_rich_positions=NULL;}
    s_rich_meta=heap_caps_calloc(1,sizeof(*s_rich_meta),MUSE_BIG_CAPS);
    if(s_rich_meta){s_rich_meta->styles=heap_caps_calloc(MUSE_RICH_STYLE_CAP,1,MUSE_BIG_CAPS);s_rich_meta->cap=MUSE_RICH_STYLE_CAP;if(!s_rich_meta->styles){free(s_rich_meta);s_rich_meta=NULL;}}
    s_reader_decor=heap_caps_calloc(1024,sizeof(*s_reader_decor),MUSE_BIG_CAPS);
    s_reader_window[0]=heap_caps_calloc(1,8192,MUSE_BIG_CAPS);s_reader_window[1]=heap_caps_calloc(1,8192,MUSE_BIG_CAPS);
    s_reader_font=lv_font_montserrat_14;s_reader_font.fallback=&muse_reader_bold_font;
    if (!s_conversation_text) return;
    s_conversation = lv_obj_create(face);
    lv_obj_remove_style_all(s_conversation);
    lv_obj_set_pos(s_conversation, 8, 46);
    lv_obj_set_size(s_conversation, s_w - 16, 144);
    lv_obj_set_style_bg_color(s_conversation, lv_color_hex(0x140e20), 0);
    lv_obj_set_style_bg_opa(s_conversation, LV_OPA_80, 0);
    lv_obj_set_style_border_color(s_conversation, lv_color_hex(0x835ace), 0);
    lv_obj_set_style_border_width(s_conversation, 1, 0);
    lv_obj_set_style_border_opa(s_conversation, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_conversation, 12, 0);
    lv_obj_set_style_shadow_width(s_conversation, 12, 0);
    lv_obj_set_style_shadow_opa(s_conversation, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(s_conversation, lv_color_black(), 0);
    lv_obj_remove_flag(s_conversation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *header = lv_button_create(s_conversation);
    s_conversation_header = header;
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, s_w - 86, 28);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_30, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x835ace), 0);
    lv_obj_add_event_cb(header, conversation_click, LV_EVENT_CLICKED, NULL);
    s_conversation_title = make_label(header, &lv_font_montserrat_14, 0xc29fff);
    lv_obj_align(s_conversation_title, LV_ALIGN_LEFT_MID, 9, 0);
    s_conversation_follow_button = lv_button_create(s_conversation);
    lv_obj_remove_style_all(s_conversation_follow_button);
    lv_obj_set_pos(s_conversation_follow_button, s_w - 158, 2);
    lv_obj_set_size(s_conversation_follow_button, 74, 24);
    lv_obj_set_style_radius(s_conversation_follow_button, 7, 0);
    lv_obj_set_style_bg_color(s_conversation_follow_button, lv_color_hex(0x9c6aff), 0);
    lv_obj_set_style_bg_opa(s_conversation_follow_button, LV_OPA_20, 0);
    lv_obj_add_event_cb(s_conversation_follow_button, conversation_click, LV_EVENT_CLICKED, (void *)2);
    s_conversation_follow_label = make_label(s_conversation_follow_button, &lv_font_montserrat_14, 0xd4bdff);
    bool follow_enabled=muse_settings_speech_follow();
    s_conversation_follow_mode=follow_enabled?2:0;
    lv_label_set_text(s_conversation_follow_label,follow_enabled?"Following":"Follow off");
    lv_obj_center(s_conversation_follow_label);
    lv_obj_t *latest = lv_button_create(s_conversation);
    s_conversation_latest_button = latest;
    lv_obj_remove_style_all(latest);
    lv_obj_set_pos(latest, s_w - 86, 0);
    lv_obj_set_size(latest, 66, 28);
    lv_obj_add_event_cb(latest, conversation_click, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *latest_label = make_label(latest, &lv_font_montserrat_14, 0xc29fff);
    lv_label_set_text(latest_label, "Latest");
    lv_obj_center(latest_label);
    s_conversation_body = lv_obj_create(s_conversation);
    lv_obj_remove_style_all(s_conversation_body);
    lv_obj_set_pos(s_conversation_body, 9, 30);
    lv_obj_set_size(s_conversation_body, s_w - 34, 105);
    lv_obj_set_scroll_dir(s_conversation_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_conversation_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_conversation_body, LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(s_conversation_body, lv_color_hex(0x9c6aff), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_conversation_body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_conversation_body, 3, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(s_conversation_body, conversation_scroll, LV_EVENT_SCROLL, NULL);
    s_reader_spacer=lv_obj_create(s_conversation_body);lv_obj_remove_style_all(s_reader_spacer);lv_obj_set_size(s_reader_spacer,1,1);lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    s_conversation_label = make_label(s_conversation_body, &lv_font_montserrat_14, 0xf3edff);
    lv_obj_set_style_text_font(s_conversation_label,&s_reader_font,0);
    lv_obj_add_event_cb(s_conversation_label,reader_draw,LV_EVENT_DRAW_MAIN_BEGIN,NULL);
    lv_obj_add_event_cb(s_conversation_label,reader_draw,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_add_flag(s_conversation_label,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_conversation_label,reader_link,LV_EVENT_CLICKED,NULL);
    lv_obj_set_width(s_conversation_label, s_w - 44);
    lv_obj_set_style_text_align(s_conversation_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_conversation_label, 4, 0);
    lv_label_set_long_mode(s_conversation_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_color(s_conversation_label, lv_color_hex(0xc19aff), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_conversation_label, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_conversation_label, lv_color_hex(0x17092b), LV_PART_SELECTED);
    conversation_render("Your conversation appears here.\nDrag to read; tap the title for more room.");
    lv_label_set_text(s_conversation_label,s_reader_full_text);
    archive_build();
    s_loading_panel=lv_obj_create(s_conversation);lv_obj_remove_style_all(s_loading_panel);lv_obj_set_pos(s_loading_panel,9,29);lv_obj_set_size(s_loading_panel,s_w-34,36);
    lv_obj_remove_flag(s_loading_panel,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(s_loading_panel,LV_OBJ_FLAG_HIDDEN);
    s_loading_bar=lv_bar_create(s_loading_panel);lv_obj_remove_style_all(s_loading_bar);lv_obj_set_size(s_loading_bar,s_w-34,23);lv_bar_set_range(s_loading_bar,0,100);lv_bar_set_value(s_loading_bar,100,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_loading_bar,lv_color_hex(0x21152e),0);lv_obj_set_style_bg_opa(s_loading_bar,255,0);lv_obj_set_style_radius(s_loading_bar,6,0);
    lv_obj_set_style_border_width(s_loading_bar,1,0);lv_obj_set_style_border_color(s_loading_bar,lv_color_hex(0x9f7bcf),0);
    lv_obj_set_style_bg_color(s_loading_bar,lv_color_hex(0x51336e),LV_PART_INDICATOR);lv_obj_set_style_bg_opa(s_loading_bar,255,LV_PART_INDICATOR);lv_obj_set_style_radius(s_loading_bar,6,LV_PART_INDICATOR);
    lv_obj_remove_flag(s_loading_bar,LV_OBJ_FLAG_CLICKABLE);
    s_loading_text=make_label(s_loading_panel,&lv_font_montserrat_14,0xf7f0ff);lv_obj_set_pos(s_loading_text,3,3);lv_obj_set_style_text_align(s_loading_text,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(s_loading_text,LV_LABEL_LONG_MODE_DOTS);
    s_loading_note=make_label(s_loading_panel,&lv_font_unscii_8,0xc5b4dc);lv_obj_set_y(s_loading_note,26);lv_obj_set_style_text_align(s_loading_note,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(s_loading_note,LV_LABEL_LONG_MODE_DOTS);
    conversation_size();
    lv_obj_update_layout(s_conversation);
    reader_window_refresh();
}

#if LV_USE_SNAPSHOT
static void reader_stress_begin(void)
{
    char *raw=heap_caps_calloc(1,24000,MUSE_BIG_CAPS);if(!raw)return;
    size_t at=0;
    for(unsigned i=0;i<90;i++){
        int n=snprintf(raw+at,24000-at,"**Paragraph %u**\nThe hallway was *dark*, but the lantern glowed. ~~Crossed out~~ `code words` stay readable.\n- First item\n  - Nested item\n> The sea was calm.\n[Link](https://example.com)\n\n",i+1);
        if(n<0 || (size_t)n>=24000-at)break;
        at+=(size_t)n;
    }
    s_conversation_preview=true;s_conversation_size_mode=2;s_conversation_follow=false;
    conversation_render(raw);free(raw);conversation_size();
    s_reader_stress_index=0;s_reader_stress_mode=0;s_reader_stress_steps=0;s_reader_stress_active=true;
}
static void reader_stress_tick(void)
{
    if(!s_reader_stress_active)return;
    if(!s_conversation_preview){s_reader_stress_active=false;return;}
    if(!reader_layout_ensure())return;
    if(s_reader_stress_index>=s_reader_point_count){
        if(++s_reader_stress_mode>=3){
            printf("@reader_stress {\"complete\":true,\"steps\":%u,\"chars\":%u,\"modes\":3,\"fallbacks\":%lu}\n",s_reader_stress_steps,(unsigned)s_reader_point_count,(unsigned long)s_reader_layout_fallbacks);fflush(stdout);
            s_reader_stress_active=false;s_conversation_preview=false;s_conversation_version=UINT32_MAX;s_conversation_size_mode=1;s_conversation_follow=true;conversation_size();return;
        }
        s_conversation_size_mode=s_reader_stress_mode==1?0:1;conversation_size();s_reader_stress_index=0;return;
    }
    unsigned end=s_reader_stress_index+4;if(end>s_reader_point_count)end=(unsigned)s_reader_point_count;
    reader_select_start(s_reader_stress_index);reader_select_end(end);
    lv_point_t pos;reader_letter_pos(s_reader_stress_index,&pos);s_conversation_scrolling=true;lv_obj_scroll_to_y(s_conversation_body,LV_MAX(0,pos.y-20),LV_ANIM_OFF);s_conversation_scrolling=false;
    s_reader_stress_index+=29;s_reader_stress_steps++;
}
#endif

static void conversation_tick(void)
{
#if LV_USE_SNAPSHOT
    if(s_reader_stress_cancel){s_reader_stress_cancel=false;s_reader_stress_active=false;}
    if(s_reader_stress_pending && s_conversation){s_reader_stress_pending=false;reader_stress_begin();return;}
    if(s_reader_stress_active){
        muse_mode_t mode=muse_state_mode(NULL);
        muse_module_llm_status_t voice;muse_module_llm_status(&voice);
        if(mode==MUSE_MODE_IDLE || mode==MUSE_MODE_ERROR || voice.state==MUSE_MODULE_LLM_DISCOVERING)return;
        s_reader_stress_active=false;s_conversation_preview=false;s_conversation_version=UINT32_MAX;
    }
    if (s_conversation && s_conversation_bench >= 0) {
        int mode = s_conversation_bench;
        s_conversation_bench = -1;
        if (!mode) {
            s_archive_visible=false;
            lv_label_set_text(s_archive_save_label,"Save");
            for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
            lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);
            s_conversation_preview = false;
            s_conversation_version = UINT32_MAX;
            s_conversation_size_mode = 1;
            s_conversation_follow = true;
            conversation_size();
        } else {
            s_conversation_preview = true;
            s_conversation_size_mode = mode == 4 ? 0 : mode == 3 ? 1 : 2;
            conversation_size();
            conversation_render(
                "You:\nShow the formatting tour.\n\nMuse:\n"
                "**Bold** *Italic* ~~Crossed out~~ `Code`\n\n"
                "- First item\n  - Nested item\n1. First step\n2. Next step\n- [x] Finished task\n- [ ] Open task\n\n"
                "> The sea was calm beside the lighthouse.\n\n"
                "[Example link](https://example.com/)\n\n"
                "The **keeper** climbed *slowly*. Keep reading.\n"
                "Smart quotes: \xe2\x80\x9cHello\xe2\x80\x9d \xe2\x80\x94 clear text.\n"
                "This is a temporary display preview.");
            s_conversation_scrolling = true;
            lv_obj_update_layout(s_conversation_body);
            lv_obj_scroll_to_y(s_conversation_body, mode == 2 ? 10000 : 0, LV_ANIM_OFF);
            s_conversation_scrolling = false;
            if (mode == 5) {
                const char *body = s_reader_full_text;
                const char *word = strstr(body, "reading");
                if (word) {
                    uint32_t at=0;for(const char *p=body;p<word;p++)if(((unsigned char)*p&0xc0)!=0x80)at++;
                    reader_select_start(at);
                    reader_select_end(at + 7);
                    lv_point_t pos;
                    reader_letter_pos(at, &pos);
                    lv_obj_scroll_to_y(s_conversation_body, LV_MAX(0, pos.y - 35), LV_ANIM_OFF);
                }
            }
            return;
        }
    }
#endif
    archive_tick();
    if(s_archive_visible)return;
    size_t latest;
    if (!s_conversation || !muse_conversation_copy(s_conversation_text, MUSE_CONVERSATION_CAP, &s_conversation_version, &latest)) return;
    s_conversation_preview = false;
    s_conversation_word_seen = UINT32_MAX;
    conversation_render(s_conversation_text[0] ? s_conversation_text :
        "Your conversation appears here.\nDrag to read; tap the title for more room.");
    lv_obj_update_layout(s_conversation_body);
    uint32_t chars = 0;
    if(s_rich_positions)chars=s_rich_positions[latest];
    else for (size_t i = 0; i < latest; i++) if (((unsigned char)s_conversation_text[i] & 0xc0) != 0x80) chars++;
    s_conversation_latest_char = chars;
    lv_point_t pos;
    reader_letter_pos(chars, &pos);
    s_conversation_latest_y = pos.y;
    if (s_conversation_follow) conversation_latest();
}

static void conversation_speech_tick(void)
{
    if((s_indev && lv_indev_get_state(s_indev)==LV_INDEV_STATE_PRESSED) || (s_tv && lv_tileview_get_tile_active(s_tv)!=s_face))return;
    if (!s_conversation) return;
    if(s_archive_visible){
        if(s_conversation_follow_mode!=3){s_conversation_follow_mode=3;lv_label_set_text(s_conversation_follow_label,"Live");}
        return;
    }
    bool enabled = muse_settings_speech_follow();
#if LV_USE_SNAPSHOT
    enabled=enabled || s_library_narration_test;
#endif
    int button_mode = !enabled ? 0 : !s_conversation_follow ? 1 : 2;
    if (button_mode != s_conversation_follow_mode) {
        s_conversation_follow_mode = button_mode;
        lv_label_set_text(s_conversation_follow_label, button_mode == 0 ? "Follow off" : button_mode == 1 ? "Resume" : "Following");
    }
    if(!reader_layout_ensure()){
        reader_select_start(LV_LABEL_TEXT_SELECTION_OFF);reader_select_end(LV_LABEL_TEXT_SELECTION_OFF);return;
    }
    if (s_conversation_preview || s_archive_visible) return;
    muse_module_llm_status_t st;
    muse_module_llm_status(&st);
    if (st.word_version == s_conversation_word_seen) return;
    if (!enabled || !st.word_active) {
        reader_select_start(LV_LABEL_TEXT_SELECTION_OFF);
        reader_select_end(LV_LABEL_TEXT_SELECTION_OFF);
        s_conversation_word_seen = st.word_version;
        return;
    }
    size_t start, end;
    if (!muse_conversation_word_position(s_conversation_version, st.word_start, st.word_end, &start, &end)) {
        reader_select_start(LV_LABEL_TEXT_SELECTION_OFF);
        reader_select_end(LV_LABEL_TEXT_SELECTION_OFF);
        return;
    }
    uint32_t char_start = 0, char_end = 0;
    if(s_rich_positions){char_start=s_rich_positions[start];char_end=s_rich_positions[end];}
    else for (size_t i = 0; i < end; i++) if (((unsigned char)s_conversation_text[i] & 0xc0) != 0x80) {
        char_end++;
        if (i < start) char_start++;
    }
    reader_select_start(char_start);
    reader_select_end(char_end);
    s_conversation_word_seen = st.word_version;
    if (s_conversation_follow) {
        lv_point_t pos;
        reader_letter_pos(char_start, &pos);
        int y = lv_obj_get_scroll_y(s_conversation_body), h = lv_obj_get_height(s_conversation_body);
        if (pos.y < y || pos.y + 20 > y + h) {
            s_conversation_scrolling = true;
            lv_obj_scroll_to_y(s_conversation_body, LV_MAX(0, pos.y - h / 3), LV_ANIM_OFF);
            s_conversation_scrolling = false;
        }
    }
}

#endif

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *face = scr;
    if (muse_board->touch) {
        /* Swipe left from Muse for settings. */
        s_tv = lv_tileview_create(scr);
        lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
        s_face = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
        /* It never scrolls, but LVGL would size its scrollbars from all its
         * children every time it draws any part of it. */
        lv_obj_set_scrollbar_mode(s_face, LV_SCROLLBAR_MODE_OFF);
        /* Match the avatar's black canvas; the tile's theme background otherwise
         * shows as a different-coloured square around the character. */
        lv_obj_set_style_bg_color(s_face, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
        s_settings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);
        /* Reader/touch updates must recognize the first page before any swipe. */
        lv_tileview_set_tile(s_tv,s_face,LV_ANIM_OFF);
        lv_obj_add_event_cb(s_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        face = s_face;
    }

    if (!s_small && muse_board->no_ring) {
        /* A square panel: progress along the bottom edge, as the compact
         * layout has it, rather than a circle around Muse. */
        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 6);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else if (!s_small) {
        /* Progress ring around the bezel. */
        int d = (s_w < s_h ? s_w : s_h) - 8;
        s_ring = lv_arc_create(face);
        lv_obj_set_size(s_ring, d, d);
        lv_obj_center(s_ring);
        lv_arc_set_bg_angles(s_ring, 0, 360);
        lv_arc_set_rotation(s_ring, 270);
        lv_arc_set_range(s_ring, 0, RING_RANGE);
        lv_arc_set_value(s_ring, 0);
        lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING_BG), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);
        lv_obj_add_event_cb(s_ring, on_ring_draw, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    }

    /*
     * Full layout, from the bottom up (offsets from the centre): two caption
     * lines as low as the ring leaves them room, the level meter, then Muse,
     * whose blank bottom rows can tuck in behind the meter.
     */
    int ring_in = (s_w < s_h ? s_w : s_h) / 2 - 10;   /* the ring's inner edge */
    int cap_h = 2 * lv_font_get_line_height(&lv_font_unscii_16) + CAPTION_LINE_SPACE;
    int cap_bottom = 179;                              /* a 466 px circle's; fine for rectangles */
    if (muse_board->round) {
        cap_bottom = (int)sqrtf((float)(ring_in * ring_in - CAPTION_W * CAPTION_W / 4)) - 3;
    }
    int cap_top = cap_bottom - cap_h;
    int meter_y = cap_top - 6 - METER_SEG_PX / 2;
    int art_bottom = meter_y - METER_SEG_PX / 2 - 4;
    s_big_y = s_small ? 0 : art_bottom - (s_canvas_px / 2 - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W));

    /* The character. */
    muse_image_init();
    s_canvas = lv_image_create(face);
    lv_image_set_src(s_canvas, &s_muse_src);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_big_y);
    s_muse_y = s_big_y;
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_clicked, LV_EVENT_CLICKED, NULL);
    if (s_ring) {
        /* The canvas's black corners reach the bezel; keep the ring on top. */
        lv_obj_move_foreground(s_ring);
    }
    build_button_icons(face);

    /* Status line: connectivity icons + power. */
    lv_obj_t *status = lv_obj_create(face);
    lv_obj_remove_style_all(status);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, s_small ? 4 : 8, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 40 : s_small ? 1 : 20 + s_dy);
    s_wifi_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_ble_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_power_lbl = make_label(status, &lv_font_unscii_8, COLOR_DIM);

    /* The compact layout leaves the state to the avatar and the caption,
     * unless the screen is tall enough to fit it in small type above Muse. */
    s_state_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_letter_space(s_state_lbl, s_small ? 1 : 2, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 64 : s_small ? 22 : 40 + s_dy);
    lv_obj_set_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    /* This gadget's own name, dim under the state while it's unpaired: with
     * more than one on the bench, the screen says which one to pick in the
     * Muse app. update_chrome() fills it in, shortens it to the hex tail on a
     * screen too narrow for the whole thing, and empties it once paired. */
    s_name_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, COLOR_DIM);
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, muse_board->rim_controls ? 88 : s_small ? 32 : 60 + s_dy);
    if (muse_board->rim_controls) {
        /* Safe chord below the round panel's top, not the full 466 px width. */
        lv_obj_set_size(s_state_lbl, 280, lv_font_get_line_height(&lv_font_unscii_16));
        lv_obj_set_size(s_name_lbl, 280, lv_font_get_line_height(&lv_font_unscii_16));
        lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_DOTS);
    }
    /* Same rule as the state label: a square 128 px screen centres Muse over
     * these rows, so there's nowhere to put this without covering the face. */
    lv_obj_set_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    s_caption_lbl = make_label(face, font_pick(caption_font(), &lv_font_unscii_8), COLOR_CAPTION);
    if (s_small) {
        /* Two lines over the bottom of the face, on a dark band so they stay
         * legible. A tall screen has room to keep them above the mic icon. */
        lv_obj_set_size(s_caption_lbl, s_w, 2 * 8 + 2 + 4);
        lv_obj_set_style_pad_ver(s_caption_lbl, 2, 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
        lv_obj_set_style_bg_color(s_caption_lbl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        /* Touch screens need the caption above the navigation dots too. */
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, (s_tall || s_tv) ? -30 : -3);
#if CONFIG_MUSE_CJK_FONT
        /* CJK doesn't fit unscii-8's cell, so a reply with CJK in it fills the
         * band one line at a time in the 16 px caption font instead. */
        muse_state_set_cjk_page(s_w / lv_font_get_glyph_width(caption_font(), 'M', ' '), 1);
#endif

        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 3);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        return;
    }
    /* Fixed height: a longer caption ends in dots rather than growing into the ring. */
    lv_obj_set_size(s_caption_lbl, CAPTION_W, cap_h);
    lv_obj_set_style_text_line_space(s_caption_lbl, CAPTION_LINE_SPACE, 0);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, cap_top + cap_h / 2);

    /* Chunky level meter. */
    int span = METER_SEGS * (METER_SEG_PX + METER_GAP_PX) - METER_GAP_PX;
    for (int i = 0; i < METER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(face);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(seg, METER_SEG_PX, METER_SEG_PX);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(COLOR_METER_OFF), 0);
        int x = -span / 2 + i * (METER_SEG_PX + METER_GAP_PX) + METER_SEG_PX / 2;
        lv_obj_align(seg, LV_ALIGN_CENTER, x, meter_y);
        s_meter[i] = seg;
    }

    build_answer(face, ring_in);
}


static void on_cover_event(lv_event_t *e)
{
#if CONFIG_MUSE_MODULE_LLM
    if(s_recorder_dark){
        if(lv_event_get_code(e)==LV_EVENT_PRESSED){
            int64_t now=esp_timer_get_time();
            if(s_recorder_tap && now-s_recorder_tap<500000){s_recorder_tap=0;muse_ui_recorder_wake();}else s_recorder_tap=now;
        }
        return;
    }
#endif
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        /* Wake on touch; keep the (now clear) cover until release so the tap
         * doesn't land on whatever is underneath. */
        muse_state_set_asleep(false);
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_TRANSP, 0);
    } else if (!muse_state_asleep()) {
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    }
}

/* With the display lock held. */
static void image_hide_locked(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_image, NULL);
        heap_caps_free(s_image_buf);
        s_image_buf = NULL;
        s_image_dsc.data = NULL;
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

/* Each frame: shows a new image and redraws what the download changed. */
static void image_sync(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf && !s_image_dsc.data) {
        s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_image_dsc.header.w = s_w;
        s_image_dsc.header.h = s_h;
        s_image_dsc.header.stride = s_w * sizeof(uint16_t);
        s_image_dsc.data_size = (size_t)s_w * s_h * sizeof(uint16_t);
        s_image_dsc.data = (const uint8_t *)s_image_buf;
        muse_menu_close();
        lv_image_set_src(s_image, &s_image_dsc);
        lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        muse_state_set_asleep(false);
    }
    if (s_image_dirty) {
        lv_obj_invalidate_area(s_image, &s_image_area);
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

static void on_image_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_WATCHER_CAMERA
    if (watcher_camera_preview_active()) {
        watcher_camera_preview_toggle();
        return;
    }
#endif
    image_hide_locked();
}

#if CONFIG_MUSE_WATCHER_CAMERA
static void on_camera_hint_clicked(lv_event_t *e)
{
    (void)e;
    if (watcher_camera_preview_active()) watcher_camera_preview_toggle();
}
#endif

static void on_any_press(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_MODULE_LLM
    s_touch_presses++;
#endif
    if (muse_board->touch_talk && muse_link_state() == MUSE_LINK_CONFIRM) {
        muse_input_touch_talk(true);
        muse_input_touch_talk(false);
    }
    muse_state_poke();
}

static lv_obj_t *make_control_icon(lv_obj_t *scr, const muse_button_hint_t *pos,
                                   const char *symbol, uint32_t color)
{
    lv_obj_t *icon = make_label(scr, &lv_font_montserrat_28, color);
    lv_obj_set_size(icon, 36, 36);
    lv_obj_set_style_bg_color(icon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon, 6, 0);
    lv_label_set_long_mode(icon, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(icon, symbol);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(icon, pos->align, pos->x, pos->y);
    return icon;
}

/* Icons only beside the physical controls, including before pairing. */
static void update_control_hints(muse_mode_t mode)
{
    if (!s_ptt_hint) return;
    /* Don't obscure settings rows/keyboards with floating physical icons. */
    bool settings = s_tv && lv_tileview_get_tile_active(s_tv) == s_settings;
    lv_obj_set_flag(s_ptt_hint, LV_OBJ_FLAG_HIDDEN, settings);
    lv_obj_set_flag(s_spk_hint, LV_OBJ_FLAG_HIDDEN, settings);
    lv_obj_set_flag(s_pwr_hint, LV_OBJ_FLAG_HIDDEN, settings);
    static int last_mode = -1;
    if ((int)mode != last_mode) {
        color_mic(s_ptt_hint, mode == MUSE_MODE_LISTENING ? 0x51cfff : 0x70b7ff);
        last_mode = (int)mode;
    }
    bool speaker_on = muse_settings_speaker_on();
    const char *speaker = speaker_on ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE;
    if (strcmp(speaker, lv_label_get_text(s_spk_hint))) {
        lv_label_set_text(s_spk_hint, speaker);
        lv_obj_set_style_text_color(s_spk_hint, lv_color_hex(speaker_on ? 0xffca4c : 0x8b763c), 0);
    }
}

static void build_overlays(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* Page dots. */
    for (int i = 0; i < 2 && s_tv; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COLOR_DOT_OFF), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, i ? 8 : -8, -14);
        s_dots[i] = d;
    }

    /* A downloaded image: over everything on the screen (and in snapshots),
     * under the pairing code and sleep cover on the top layer. */
    s_image = lv_image_create(scr);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
#if CONFIG_MUSE_WATCHER_CAMERA
    s_camera_hint = lv_btn_create(scr);
    lv_obj_set_size(s_camera_hint, 244, 46);
    lv_obj_align(s_camera_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_camera_hint, lv_color_hex(0x1d1735), 0);
    lv_obj_set_style_bg_opa(s_camera_hint, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_camera_hint, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_camera_hint, 2, 0);
    lv_obj_set_style_radius(s_camera_hint, 18, 0);
    lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint_text = lv_label_create(s_camera_hint);
    lv_label_set_text(hint_text, "TAP TO TAKE PHOTO");
    lv_obj_center(hint_text);
    lv_obj_add_event_cb(s_camera_hint, on_camera_hint_clicked, LV_EVENT_CLICKED, NULL);
#endif

    if (muse_board->rim_controls) {
        /* Dedicated ownership: legacy chrome/reply hides must never claim it. */
        s_ptt_hint = make_mic(scr, 26);
        lv_obj_align(s_ptt_hint, muse_board->talk_hint.align, muse_board->talk_hint.x, muse_board->talk_hint.y);
        s_spk_hint = make_control_icon(scr, &muse_board->speaker_hint, LV_SYMBOL_VOLUME_MID, 0xffca4c);
        s_pwr_hint = make_control_icon(scr, &muse_board->power_hint, LV_SYMBOL_POWER, 0xff7070);
        update_control_hints(MUSE_MODE_BOOT);
    }

    /* BLE pairing code, or the Muse app's ask for the talk button: a centred
     * column in one typeface, the code large. The small card grows with the hint. */
    s_pair = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pair);
    lv_obj_set_size(s_pair, s_small ? s_w - 8 : 300, s_small ? LV_SIZE_CONTENT : 150);
    lv_obj_center(s_pair);
    lv_obj_set_flex_flow(s_pair, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_pair, s_small ? 10 : 18, 0);
    lv_obj_set_style_pad_hor(s_pair, s_small ? 6 : 16, 0);
    lv_obj_set_style_pad_row(s_pair, s_small ? 4 : 10, 0);
    lv_obj_set_style_radius(s_pair, s_small ? 10 : 24, 0);
    lv_obj_set_style_bg_opa(s_pair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(0x171230), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    s_pair_title = make_label(s_pair, font_pick(&lv_font_montserrat_20, FONT_COMPACT), COLOR_LIT);
    lv_label_set_text(s_pair_title, "Pairing code");
    s_pair_code = make_label(s_pair, font_pick(&lv_font_montserrat_28, &lv_font_montserrat_20), COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_pair_code, s_small ? 2 : 6, 0);
    s_pair_hint = make_label(s_pair, font_pick(&lv_font_montserrat_14, FONT_COMPACT), COLOR_DIM);
    lv_label_set_text(s_pair_hint, s_small ? "Enter on phone" : "Enter it on your phone");
    /* Wraps: "bottom right button" is wider than the AIPI's card. */
    lv_obj_set_width(s_pair_hint, lv_pct(100));
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_MODE_WRAP);

    /* Sleep cover: swallows the waking touch. */
    s_cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_cover, lv_color_black(), 0);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESS_LOST, NULL);

    if (s_indev) {
        lv_indev_add_event_cb(s_indev, on_any_press, LV_EVENT_PRESSED, NULL);
    }
#if CONFIG_MUSE_MODULE_LLM
    playback_button_build();
#endif
}

static void apply_brightness(int pct)
{
    if (pct != s_brightness) {
        muse_board->set_brightness(pct);
        s_brightness = pct;
    }
}

bool muse_ui_dark(void)
{
    return s_dark;
}

/* Returns true while asleep (skip rendering). */
static bool update_sleep(void)
{
    bool asleep = muse_state_asleep() || s_recorder_dark;
    if (asleep && !s_dark) {
        muse_menu_close();
        apply_brightness(0);
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(true);
        }
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        s_dark = true;
    } else if (!asleep && s_dark) {
        s_dark = false;
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(false);
        }
        /* Woken by a button: drop the cover now. Woken by touch: on release. */
        if (lv_obj_get_style_bg_opa(s_cover, 0) == LV_OPA_COVER) {
            lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_dark) {
        int target = muse_settings_brightness();
        if (s_preview_brightness >= 0) {
            if (s_preview_brightness == target) {
                s_preview_brightness = -1;
            } else {
                target = s_preview_brightness;
            }
        }
        apply_brightness(target);
    }
    return s_dark;
}

/* Idle's label: whether a press reaches Hatch now or waits for Wi-Fi. */
static const char *idle_name(muse_wifi_state_t wifi)
{
    static bool joined;   /* since boot: from then on, a drop is reconnecting */
    switch (wifi) {
    case MUSE_WIFI_CONNECTED:
        joined = true;
        return MODE_NAMES[MUSE_MODE_IDLE];
    case MUSE_WIFI_OFF:
        return "WI-FI OFF";
    case MUSE_WIFI_NO_NETWORK:
        return "SET UP WI-FI";
    case MUSE_WIFI_NOT_NEARBY:
        return "NO WI-FI";   /* none of the saved networks is in range */
    default:
        return joined ? "RECONNECTING" : "CONNECTING";
    }
}

static void update_chrome(float now)
{
    if (now < s_next_settings_tick) {
        return;
    }
    s_next_settings_tick = now + SETTINGS_TICK_S;

    if (s_tv) {
        int page = lv_tileview_get_tile_active(s_tv) == s_settings;
        bool subpage = muse_settings_ui_in_subpage();
        bool swipe = !page || !subpage;
        if (swipe != lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE)) {
            lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, swipe);
        }
        bool full_reader=false;
#if CONFIG_MUSE_MODULE_LLM
        full_reader=!page && s_conversation && s_conversation_size_mode==2 && !lv_obj_has_flag(s_conversation,LV_OBJ_FLAG_HIDDEN);
#endif
        bool transport=false;
#if CONFIG_MUSE_MODULE_LLM
        transport=!page && s_transport_visible;
#endif
        int shown = page * 2 + subpage + (full_reader?4:0) + (transport?8:0);
        if (shown != s_shown_page) {
            for (int i = 0; i < 2; i++) {
                lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COLOR_ACCENT : COLOR_DOT_OFF), 0);
                lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, (page && subpage) || full_reader || transport);
            }
            s_shown_page = shown;
        }
        muse_settings_ui_tick(lv_obj_get_scroll_x(s_tv) > 0);
    }

    /* Joining, the icon blinks: the compact layout has no state label. */
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool joining = w.state == MUSE_WIFI_CONNECTING || w.state == MUSE_WIFI_FAILED;
    const char *wifi = w.state == MUSE_WIFI_CONNECTED ||
                       (joining && (muse_board->epaper || (int)(now * 2) % 2 == 0)) ? LV_SYMBOL_WIFI : "";
    if (strcmp(wifi, lv_label_get_text(s_wifi_icon)) != 0) {
        lv_label_set_text(s_wifi_icon, wifi);
    }
    s_idle_name = idle_name(w.state);
#if CONFIG_MUSE_MODULE_LLM
    status_header_update(&w);
#endif
    muse_ble_status_t b;
    muse_ble_status(&b);
    const char *ble = b.state != MUSE_BLE_OFF ? LV_SYMBOL_BLUETOOTH : "";
    if (strcmp(ble, lv_label_get_text(s_ble_icon)) != 0) {
        lv_label_set_text(s_ble_icon, ble);
        lv_obj_set_style_text_color(s_ble_icon, lv_color_hex(b.state == MUSE_BLE_CONNECTED ? COLOR_ACCENT : COLOR_DIM), 0);
    }

    /* Paired, the name has done its job (picking this one out in the Muse
     * app) and the speaker button has replies to mute. */
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    bool paired = h.state != MUSE_HATCH_NOT_SET;

    /* The gadget's name, until it's paired. Emptied rather than hidden: the
     * read layout unhides it on the way out. A narrow screen gets the hex tail
     * on its own, which is the part that differs between two of them, rather
     * than a head that ends in dots before it gets there. */
    const lv_font_t *name_font = s_small ? &lv_font_unscii_8 : &lv_font_unscii_16;
    int name_cw = lv_font_get_glyph_width(name_font, 'M', ' ');
    const char *shown = paired ? "" : b.name;
#if LV_USE_SNAPSHOT
    if (!paired && s_bench_demo) shown = "MuseGadget-DEMO";
#endif
    if (name_cw > 0 && (int)strlen(shown) * name_cw > (muse_board->rim_controls ? 280 : s_w)) {
        const char *tail = strrchr(shown, '-');
        if (tail && tail[1]) {
            shown = tail + 1;
        }
    }
    if (strcmp(shown, lv_label_get_text(s_name_lbl)) != 0) {
        lv_label_set_text(s_name_lbl, shown);
    }

    /* The same card asks for the talk button when the Muse app pairs. */
    bool confirm = !b.passkey && muse_link_state() == MUSE_LINK_CONFIRM;
    if (b.passkey || confirm) {
        char code[24], hint[40];
        if (confirm) {
            if (muse_board->touch_talk || !muse_board->audio_init) {
                strlcpy(code, "Tap screen", sizeof(code));
                strlcpy(hint, "Tap to confirm pairing", sizeof(hint));
            } else {
                strlcpy(code, s_small ? "Press" : "Press button", sizeof(code));
                snprintf(hint, sizeof(hint), s_small ? "%s button" : "Press the %s button", muse_board->talk_button);
            }
        } else {
            snprintf(code, sizeof(code), "%06lu", (unsigned long)b.passkey);
            strlcpy(hint, s_small ? "Enter on phone" : "Enter it on your phone", sizeof(hint));
        }
        const char *title = confirm ? (s_small ? "Muse app" : "Pair with Muse app") : "Pairing code";
        if (strcmp(code, lv_label_get_text(s_pair_code)) != 0) {
            lv_label_set_text(s_pair_code, code);
            lv_label_set_text(s_pair_title, title);
            lv_label_set_text(s_pair_hint, hint);
        }
    }
    lv_obj_set_flag(s_pair, LV_OBJ_FLAG_HIDDEN, !b.passkey && !confirm);

    bool speaker = muse_settings_speaker_on();   /* also set from settings, the phone and serial */
    if (s_speaker && (int)speaker != s_shown_speaker) {
        show_speaker(speaker);
    }
    bool output = muse_board->audio_init || HAS_MODULE_AUDIO;
    if (s_speaker && (paired && output) == lv_obj_has_flag(s_speaker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_speaker, LV_OBJ_FLAG_HIDDEN, !paired || !output);
    }
    /* Unpaired, a press only says "SET UP MUSE FIRST", so the mic goes too.
     * While a reply's layout is up it decides; that's only ever paired. A
     * microphone without a speaker (mic_init) still talks. */
    bool mic = paired && (muse_board->audio_init || muse_board->mic_init || HAS_MODULE_AUDIO);
    if (s_mic_icon && !muse_board->rim_controls && s_answer < 0 && mic == lv_obj_has_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, !mic);
    }
}

/* One area for the whole meter before its segments change: LVGL won't join
 * squares with gaps between them, and each area it keeps costs a walk of the
 * widget tree. The segments' own areas then fall inside this one. */
static void invalidate_meter(void)
{
    lv_area_t a, last;
    lv_obj_get_coords(s_meter[0], &a);
    lv_obj_get_coords(s_meter[METER_SEGS - 1], &last);
    a.x2 = last.x2;
    lv_obj_invalidate_area(lv_obj_get_parent(s_meter[0]), &a);
}

static void set_meter_visible(bool visible)
{
    if (s_small || visible == s_meter_visible) {
        return;
    }
    invalidate_meter();
    for (int i = 0; i < METER_SEGS; i++) {
        if (visible) {
            lv_obj_remove_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_meter_visible = visible;
    s_shown_lit = -1;
}

static void update_power(float now)
{
    if (now < s_next_power_update) {
        return;
    }
    s_next_power_update = now + 1.0f;

    muse_power_t p = muse_state_power();
    char buf[32];
    if (p.battery_pct < 0) {
        strlcpy(buf, p.usb ? (s_small ? "USB" : "USB POWER") : "", sizeof(buf));
    } else if (s_small) {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? "+" : "", p.battery_pct);
    } else if (p.charging) {
        snprintf(buf, sizeof(buf), "CHARGING %d%%", p.battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "BATTERY %d%%", p.battery_pct);
    }
    if (strcmp(buf, lv_label_get_text(s_power_lbl)) != 0) {
        lv_label_set_text(s_power_lbl, buf);
    }
}

#if CONFIG_MUSE_MODULE_LLM
static void loading_tick(muse_mode_t mode,float now)
{
    static bool tracking,learned,rebuffering,save_pending;static float started,paused_at,paused_seconds,ready_until,next,local_at;
    static char key[25];static muse_wait_countdown_t countdown;
    if(!s_loading_panel||now<next)return;
    next=now+.1f;
    muse_module_llm_status_t st;muse_module_llm_status(&st);
    muse_native_voice_status_t p;muse_native_voice_status(&p);
    bool speaking=st.state==MUSE_MODULE_LLM_SPEAKING;
    bool waiting=(mode==MUSE_MODE_THINKING||st.state==MUSE_MODULE_LLM_PREPARING)&&!muse_memory_recorder_active();
    if(waiting&&!tracking){tracking=true;learned=false;rebuffering=p.opening_ready_ms&&st.state==MUSE_MODULE_LLM_PREPARING;started=now;paused_at=paused_seconds=ready_until=local_at=0;key[0]=0;countdown=(muse_wait_countdown_t){0};}
    if(tracking&&p.active&&strcmp(key,p.job)){if(key[0])countdown=(muse_wait_countdown_t){0};strlcpy(key,p.job,sizeof(key));if(!rebuffering)started=now-p.elapsed_ms/1000.f;}
    bool cloud=key[0]&&!strcmp(key,p.job)&&muse_settings_voice_source()==1;
    if(tracking&&speaking&&!ready_until){
        ready_until=now+.7f;
        if(!learned&&!rebuffering&&cloud&&!p.recoveries&&p.source_ready_ms>2000&&p.opening_ready_ms>p.source_ready_ms&&p.words_ready){
            muse_wait_learn(muse_wait_model(),p.source_ready_ms/1000.f,(p.opening_ready_ms-p.source_ready_ms)/1000.f,p.opening_words);save_pending=true;learned=true;
        }
    }
    bool visible=tracking&&(waiting||(ready_until&&now<ready_until))&&!s_archive_visible;
    if(!waiting&&(!ready_until||now>=ready_until)){tracking=false;visible=false;}
    if(save_pending&&!tracking&&mode==MUSE_MODE_IDLE&&st.state==MUSE_MODULE_LLM_READY){muse_wait_save();save_pending=false;}
    if(s_loading_visible!=visible){s_loading_visible=visible;lv_obj_set_flag(s_loading_panel,LV_OBJ_FLAG_HIDDEN,!visible);conversation_size();}
    if(!visible)return;
    bool paused=st.paused||st.pause_requested;
    if(paused&&!paused_at)paused_at=now;
    if(!paused&&paused_at){paused_seconds+=now-paused_at;paused_at=0;}
    float elapsed=(paused_at?paused_at:now)-started-paused_seconds;
    muse_wait_model_t *model=muse_wait_model();
    float budget=cloud?(p.source_ready_ms?p.source_ready_ms/1000.f:model->response_s)+muse_wait_budget(model,p.opening_words):model->response_s+muse_wait_budget(model,0);
    if(!cloud&&st.state==MUSE_MODULE_LLM_PREPARING){if(!local_at)local_at=elapsed;budget=local_at+muse_wait_budget(model,0);}
    if(rebuffering)budget=muse_wait_budget(model,18);
    s_loading_view=muse_wait_countdown(&countdown,elapsed,budget,speaking);
    const char *step="Waiting for Muse";
    if(st.state==MUSE_MODULE_LLM_PREPARING)step=cloud&&p.opening_ready_ms?"Buffering next sentence":"Preparing local voice";
    if(cloud&&p.source_ready_ms&&!rebuffering)step=p.detail==2?"Downloading audio":p.detail==3?"Checking audio":"Creating and timing voice";
    if(cloud&&p.phase==7)step="Reconnecting audio";
    char text[96],note[96];
    if(speaking){strlcpy(text,"Ready to play | 0% left",sizeof(text));strlcpy(note,"Opening audio checked and buffered",sizeof(note));}
    else if(paused){strlcpy(text,"Paused | waiting to continue",sizeof(text));snprintf(note,sizeof(note),"%u%% left | estimate paused",s_loading_view.percent);}
    else{
        if(s_loading_view.late)snprintf(text,sizeof(text),"%s | %us elapsed",step,(unsigned)elapsed);
        else snprintf(text,sizeof(text),"%s | ~%us",step,s_loading_view.seconds);
        if(cloud&&p.words_total)snprintf(note,sizeof(note),"%u%% left* | %u words | %u/%u parts",s_loading_view.percent,p.words_total,p.sections_ready,p.sections_total);
        else snprintf(note,sizeof(note),"%u%% left* | %s",s_loading_view.percent,s_loading_view.late?"Taking longer than estimated":"Estimated; waiting for reply");
    }
    if(lv_bar_get_value(s_loading_bar)!=(int)s_loading_view.percent)lv_bar_set_value(s_loading_bar,s_loading_view.percent,LV_ANIM_OFF);
    if(strcmp(lv_label_get_text(s_loading_text),text))lv_label_set_text(s_loading_text,text);
    if(strcmp(lv_label_get_text(s_loading_note),note))lv_label_set_text(s_loading_note,note);
}
#endif

static void update_status(muse_mode_t mode, float now)
{
    uint32_t accent = muse_pixel_accent(mode);
    const char *name = mode == MUSE_MODE_IDLE ? s_idle_name : MODE_NAMES[mode];
#if CONFIG_MUSE_MODULE_LLM
    loading_tick(mode,now);
    static float preparing_since;static char preparing_label[48];
    muse_module_llm_status_t voice_state;muse_module_llm_status(&voice_state);
    if(voice_state.state==MUSE_MODULE_LLM_PREPARING){
        if(!preparing_since)preparing_since=now;
        snprintf(preparing_label,sizeof(preparing_label),"PREPARING VOICE %us",(unsigned)(now-preparing_since));name=preparing_label;
    }else preparing_since=0;
    /* Wi-Fi connectivity alone must never advertise a usable microphone. */
    if(mode==MUSE_MODE_IDLE || mode==MUSE_MODE_BOOT || voice_state.state==MUSE_MODULE_LLM_DISCOVERING){
        if(voice_state.state==MUSE_MODULE_LLM_DISCOVERING)name="LOADING VOICE";
        else if(voice_state.state==MUSE_MODULE_LLM_CANCELLING)name="STARTING MIC";
        else if(voice_state.state!=MUSE_MODULE_LLM_READY)name="VOICE NOT READY";
        else if(mode==MUSE_MODE_IDLE && !strcmp(name,"READY")){
            muse_hatch_status_t connection;muse_hatch_status(&connection);
            if(connection.state==MUSE_HATCH_NOT_SET)name="PAIR MUSE";
            else if(connection.state==MUSE_HATCH_UNREACHABLE)name="MUSE OFFLINE";
            else if(connection.state!=MUSE_HATCH_REACHABLE)name="CONNECTING MUSE";
        }
    }
#endif

#if CONFIG_MUSE_MODULE_LLM
    muse_module_llm_status_t playback_status;muse_module_llm_status(&playback_status);
    muse_native_voice_status_t native;muse_native_voice_status(&native);
    if(native.active && mode==MUSE_MODE_THINKING){
        if(native.phase==7)snprintf(preparing_label,sizeof(preparing_label),"RECOVERING VOICE %u/2",native.recoveries);
        else if(native.phase==2 && native.total)snprintf(preparing_label,sizeof(preparing_label),"MUSE AUDIO %u%%",(unsigned)((uint64_t)native.received*100/native.total));
        else if(native.phase==2)snprintf(preparing_label,sizeof(preparing_label),"BUFFERING VOICE %uKB",native.received/1024);
        else if(native.phase==3)snprintf(preparing_label,sizeof(preparing_label),"CHECKING AUDIO");
        else if(native.source_ready_ms)snprintf(preparing_label,sizeof(preparing_label),"%s",native.detail==2?"DOWNLOADING VOICE":native.detail==3?"CHECKING AUDIO":"PREPARING VOICE");
        else snprintf(preparing_label,sizeof(preparing_label),"WAITING FOR MUSE %us",native.elapsed_ms/1000);
        name=preparing_label;
    }
    if(!native.active && native.phase==5 && mode==MUSE_MODE_IDLE && muse_settings_voice_source()==1 && muse_chat_playback_can_restart())name=muse_native_voice_error_title();
    if(playback_status.pause_requested)name=playback_status.paused?"PAUSED":"PAUSING";
    else if(playback_status.paused)name="RESUMING";
    if(muse_memory_recorder_active())name="RECORDING";
#endif
#if CONFIG_MUSE_MODULE_LLM && LV_USE_SNAPSHOT
    if(s_status_header && s_status_demo_pct!=-2)name="STATUS PREVIEW";
#endif
    if (strcmp(name,lv_label_get_text(s_state_lbl))) {
        lv_label_set_text(s_state_lbl, name);
        s_shown_name = name;
    }
    if ((int)mode != s_shown_state) {
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(accent), 0);
        if (s_ring) {
            lv_obj_set_style_arc_color(s_ring, lv_color_hex(accent), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(s_bar, lv_color_hex(accent), 0);
        }
        set_mic_color(mode == MUSE_MODE_LISTENING ? accent : COLOR_DIM);   /* legacy face icon */
        s_shown_state = (int)mode;
        s_shown_lit = -1;
    }

    /* Ring: phase progress while listening/speaking, a spinner while thinking. */
    int ring = 0;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        ring = (int)(muse_state_progress() * RING_RANGE);
    } else if (mode == MUSE_MODE_THINKING) {
        ring = RING_RANGE / 6;
    }
    if (s_ring && mode == MUSE_MODE_THINKING) {
        /* Move the indicator rather than rotate the arc: a rotation redraws
         * the whole ring, and with it the whole screen. */
        int start = (int)(now * 300.0f) % 360;
        lv_arc_set_angles(s_ring, start, start + ring * 360 / RING_RANGE);
        s_ring_value = -1;
    } else if (s_ring) {
        if (s_ring_value < 0) {
            lv_arc_set_bg_start_angle(s_ring, 0);   /* unchanged, but puts the value's angles back */
        }
        if (ring != s_ring_value) {
            lv_arc_set_value(s_ring, ring);
            s_ring_value = ring;
        }
    } else {
        /* Bar along the bottom edge; a sliding segment while thinking. */
        int x = mode == MUSE_MODE_THINKING ? (int)(now * s_w) % s_w : 0;
        int w = ring * s_w / RING_RANGE;
        if (ring != s_ring_value || x) {
            lv_obj_set_width(s_bar, w);
            lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, x, 0);
            s_ring_value = ring;
        }
    }

    /* The mic's meter; a reply's page has the room while answering. */
    bool meter = mode == MUSE_MODE_LISTENING;
    set_meter_visible(meter);
    if (meter && !s_small) {
        int lit = (int)lroundf(s_level * METER_SEGS);
        if (lit != s_shown_lit || accent != s_shown_accent) {
            invalidate_meter();
            for (int i = 0; i < METER_SEGS; i++) {
                /* Outer segments light last, like a centred VU. */
                int rank = abs(2 * i - (METER_SEGS - 1)) / 2;
                bool on = rank < (lit + 1) / 2;
                lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(on ? accent : COLOR_METER_OFF), 0);
            }
            s_shown_lit = lit;
            s_shown_accent = accent;
        }
    }

    static char caption[MUSE_CAPTION_MAX];
    bool fresh = muse_state_caption(caption, sizeof(caption), &s_caption_version);
    int answer = -1;
    if (s_reply_lbl) {
        /* The speaker picks the layout, even mid-reply: the voice task pages to fit. */
        int layout = (muse_board->audio_init || HAS_MODULE_AUDIO) && muse_settings_speaker_on() ? ANSWER_HEARD : ANSWER_READ;
        if (layout != s_page_for) {
            muse_state_set_page(s_answers[layout].cols, s_answers[layout].lines);
            s_page_for = layout;
        }
        if (mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING) {
            answer = layout;
        }
    }
    if (answer != s_answer) {
        set_answer(answer);
        fresh = true;   /* the caption moves between labels */
    }
    if (fresh) {
        lv_obj_t *lbl = answer >= 0 ? s_reply_lbl : s_caption_lbl;
#if CONFIG_MUSE_CJK_FONT
        if (s_small) {
            lv_obj_set_style_text_font(s_caption_lbl, muse_text_has_cjk(caption) ? caption_font() : &lv_font_unscii_8, 0);
        }
#endif
        lv_label_set_text(lbl, caption);
        lv_obj_set_flag(lbl, LV_OBJ_FLAG_HIDDEN, !caption[0]);
        if (s_reply_lbl) {
            lv_obj_add_flag(answer >= 0 ? s_caption_lbl : s_reply_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_power(now);
}

static volatile bool s_snapshot;
#if LV_USE_SNAPSHOT
static volatile int s_bench_page = -1;
void muse_ui_bench_sound(void) { s_bench_page = 2; }
void muse_ui_bench_speed(void){s_bench_page=3;}
void muse_ui_bench_page(bool settings)
{
    s_bench_page = settings ? 1 : 0;   /* applied in the LVGL task */
}
#endif

/* Streams the screen over the USB cable as base64 RGB565 (bench testing; needs
 * LV_USE_SNAPSHOT, which devices/sdkconfig.muse-bench turns on). */
static void send_snapshot(void)
{
#if LV_USE_SNAPSHOT
    lv_obj_t *target=lv_screen_active();
#if CONFIG_MUSE_MODULE_LLM
    if(s_status_dialog)target=s_status_dialog;
    if(s_library_dialog)target=s_library_dialog; /* Capture the modal, not private rows behind it. */
#endif
    lv_draw_buf_t *buf = lv_snapshot_take(target, LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        return;
    }
    enum { RAW = 144 };   /* lines must fit the driver's 256-byte TX ring */
    char hdr[48];
    int n = snprintf(hdr, sizeof(hdr), "\nSNAP BEGIN %d %d %d\n", (int)buf->header.w, (int)buf->header.h, RAW);
    muse_console_write(hdr, n);
    static unsigned char b64[4 * RAW / 3 + 4];
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const unsigned char *p = buf->data + y * buf->header.stride;
        size_t left = buf->header.w * 2;
        while (left) {
            size_t chunk = left > RAW ? RAW : left, olen;
            mbedtls_base64_encode(b64, sizeof(b64), &olen, p, chunk);
            b64[olen++] = '\n';
            muse_console_write(b64, olen);
            p += chunk;
            left -= chunk;
        }
    }
    muse_console_write("SNAP END\n", 9);
    lv_draw_buf_destroy(buf);
#else
    muse_console_write("\nSNAP OFF\n", 10);   /* so snap.py can say why */
#endif
}

void muse_ui_request_snapshot(void)
{
    s_snapshot = true;
}

static atomic_int s_navigation;
void muse_ui_navigation(int direction){atomic_store(&s_navigation,direction<0?-1:1);}
static void navigation_tick(void){
 int direction=atomic_exchange(&s_navigation,0);if(!direction || !s_tv)return;
#if CONFIG_MUSE_MODULE_LLM
 if(s_status_dialog){if(direction<0)status_popup_close(NULL);return;}
 if(s_library_dialog){if(direction<0)library_close();return;}
 if(s_archive_visible && lv_tileview_get_tile_active(s_tv)==s_face){
  if(direction>0){if(s_archive_view.library){if(s_archive_view.more)muse_memory_action("list",NULL,s_archive_view.library_offset+5);}else if(s_archive_view.page+1<s_archive_view.pages)muse_memory_action("page",s_archive_view.chat,s_archive_view.page+1);return;}
  s_archive_visible=false;lv_label_set_text(s_archive_save_label,"Save");for(unsigned i=0;i<5;i++)archive_row_visible(i,false);
  lv_obj_remove_flag(s_conversation_label,LV_OBJ_FLAG_HIDDEN);if(s_reader_spacer)lv_obj_remove_flag(s_reader_spacer,LV_OBJ_FLAG_HIDDEN);s_conversation_version=UINT32_MAX;s_conversation_follow=true;conversation_size();return;
 }
#endif
 if(muse_menu_is_open()){if(direction<0)muse_menu_close();return;}
 if(lv_tileview_get_tile_active(s_tv)==s_settings){
  if(!muse_settings_ui_navigation(direction) && direction<0)lv_tileview_set_tile(s_tv,s_face,LV_ANIM_OFF);
 }else if(direction>0)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);
}

static void recorder_tick(void){
#if CONFIG_MUSE_MODULE_LLM
 static char dark_session[33];static bool was_active;static int64_t poll_at;
 muse_recorder_view_t rec;muse_memory_recorder_copy(&rec);bool wake=atomic_exchange(&s_recorder_wake,false);
 if(rec.active){
  if(esp_timer_get_time()>poll_at){poll_at=esp_timer_get_time()+1000000;muse_memory_recorder_action("recstatus",0,0);}
  if(!strcmp(rec.state,"recording") && strcmp(dark_session,rec.id)){strlcpy(dark_session,rec.id,33);s_recorder_dark=!wake;}
 }else if(was_active){wake=true;}
 if(wake){s_recorder_dark=false;muse_state_set_asleep(false);if(s_tv)lv_tileview_set_tile(s_tv,s_settings,LV_ANIM_OFF);muse_settings_ui_open_recorder();muse_state_poke();}
 was_active=rec.active;
#endif
}

static void frame_tick(lv_timer_t *timer)
{
    recorder_tick();
    navigation_tick();
#if CONFIG_MUSE_MODULE_LLM
    playback_button_tick();
    usage_popup_tick();
    bool touching=s_indev && lv_indev_get_state(s_indev)==LV_INDEV_STATE_PRESSED;
    s_reader_phase=1;s_reader_frames++;
    bool reader_visible=!s_tv || lv_tileview_get_tile_active(s_tv)==s_face;
    if(!touching && reader_visible)conversation_tick();
#if LV_USE_SNAPSHOT
    library_test_tick();
#endif
    if(!touching && reader_visible && s_conversation_label && s_reader_decor_dirty){
        bool ready=reader_layout_ensure();if(ready){s_reader_phase=11;reader_decor_build();lv_obj_invalidate(s_conversation_label);}
    }
    s_reader_phase=2;conversation_speech_tick();
#if LV_USE_SNAPSHOT
    reader_stress_tick();
#endif
    if(!touching && reader_visible)reader_window_refresh();
    s_reader_phase=0;s_reader_frame_at=(uint32_t)(esp_timer_get_time()/1000);
#endif
#if LV_USE_SNAPSHOT
    if (s_bench_page >= 0) {
        bool sound = s_bench_page == 2;
        bool speed = s_bench_page == 3;
        bool settings = s_bench_page != 0;
        s_bench_page = -1;
        if (s_tv) lv_tileview_set_tile(s_tv, settings ? s_settings : s_face, LV_ANIM_OFF);
        if (sound) muse_settings_ui_bench_sound();
        if (speed) muse_settings_ui_bench_speed();
    }
#endif
    update_control_hints(muse_state_mode(NULL));
    if (s_snapshot) {
        s_snapshot = false;
        send_snapshot();
    }
    (void)timer;
    image_sync();
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float now = (float)esp_timer_get_time() / 1e6f;

    if (mode != s_last_mode) {
        if (mode == MUSE_MODE_LISTENING) {
            image_hide_locked();
            muse_ui_show_face();
        }
        s_last_mode = mode;
    }
    if (update_sleep()) {
        muse_voice_set_monitor(false);
        return;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_tick()) {
        muse_menu_close();
        return;   /* video covers everything */
    }
#endif
    update_chrome(now);
    if (muse_menu_tick(now)) {
        image_hide_locked();
        return;   /* the menu covers the face */
    }
    if (s_image_dsc.data) {
        return;   /* the image covers the face */
    }
    if (s_tv && lv_obj_get_scroll_x(s_tv) != 0) {
        /* Off screen, or sliding to or from settings: hold still so the
         * slide gets the whole frame time. */
        return;
    }

    /* Prioritize touch and scrolling; the restored translucent reader keeps
     * the avatar visible and animated in every reader size. */
#if CONFIG_MUSE_MODULE_LLM
    if(touching){
        update_status(mode,now);return;
    }
#endif
    /* Fast attack, slow release keeps the mouth and meter lively but readable. */
    float level = muse_state_level();
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);

    muse_pose_t pose = {
        .mode = mode,
        .t = now,
        .mode_t = mode_t,
        .level = s_level,
        .happy = muse_state_happiness(),
    };
    if (!muse_board->epaper || mode != s_last_epaper_pose) {
        muse_pixel_render(&pose);
        invalidate_muse();
        s_last_epaper_pose = mode;
    }

    update_status(mode, now);
}

esp_err_t muse_ui_start(void)
{
    s_w = muse_board->width;
    s_h = muse_board->height;
    /* The full layout assumes room for the 466 px board's header and bottom
     * captions. Short landscape panels (BOX-3) need the compact layout too,
     * as does anything narrower than its fixed 256 px captions. */
    bool short_landscape = s_w > s_h && s_h < 320;
    s_small = s_h < 200 || s_w < 200 || short_landscape || s_w < 300;
    s_tall = s_small && s_h >= s_w + 64;
    /* Small screens keep room for the status line and button icons. A narrow
     * one is as wide as Muse gets, in whole pixels. */
    s_canvas_px = s_small ? s_h * 3 / 4 : MUSE_PX_W * 5;
    if (short_landscape) {
        /* Leave the header's first 40 rows and bottom captions clear. */
        s_canvas_px = s_h * 2 / 3;
    }
    if (muse_board->avatar_px > 0) {
        s_canvas_px = muse_board->avatar_px;
    }
    if (s_canvas_px > s_w) {
        s_canvas_px = s_w / MUSE_PX_W * MUSE_PX_W;
    }
    s_dy = (s_h - 466) / 2;
    if (!s_small && muse_board->round && s_dy < 0) {
        /* A shorter screen moves the labels out towards its edges, but a
         * smaller circle's edges curve away from them. Keep them where they
         * are and shrink Muse to whole pixels that fit between them. */
        s_canvas_px = (MUSE_PX_W * 5 + 2 * s_dy) / MUSE_PX_W * MUSE_PX_W;
        s_dy = 0;
    }

    lv_display_t *disp = muse_board->display_start(&s_indev);
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    s_image_mutex = xSemaphoreCreateMutex();
    muse_board->display_lock(-1);
    build_screen();
#if CONFIG_MUSE_MODULE_LLM
    lv_display_add_event_cb(lv_display_get_default(),reader_display_event,LV_EVENT_ALL,NULL);
    muse_memory_init();
    muse_conversation_init();
    conversation_build(s_face ? s_face : lv_screen_active());
    status_header_build(s_face ? s_face : lv_screen_active());
#endif
    if (s_settings) {
        muse_settings_ui_build(s_settings);
    } else {
        muse_menu_build(lv_screen_active(), s_w, s_h);
    }
    build_overlays();
#if CONFIG_MUSE_MODULE_LLM
    muse_startup_show(false);
#endif
    lv_timer_create(frame_tick, muse_board->frame_ms, NULL);
    s_ready = true;
    muse_board->display_unlock();

    ESP_LOGI(TAG, "UI up: %dx%d, %d px Muse, %d ms frames", s_w, s_h, s_canvas_px, muse_board->frame_ms);
    return ESP_OK;
}

void muse_ui_show_face(void)
{
    if (!s_tv) {
        return;
    }
    lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
    lv_tileview_set_tile(s_tv, s_face, muse_board->epaper ? LV_ANIM_OFF : LV_ANIM_ON);
}

void muse_ui_set_swipe_enabled(bool enabled)
{
    if (!s_tv) {
        return;
    }
    lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, enabled);
    s_shown_page = -1;
    s_next_settings_tick = 0;
}

void muse_ui_preview_brightness(int pct)
{
    s_preview_brightness = pct;
    apply_brightness(pct);
}

bool muse_ui_image_size(int *w, int *h)
{
    if (!s_ready || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        return false;
    }
    *w = s_w;
    *h = s_h;
    return true;
}

int muse_ui_image_bits(void)
{
    return muse_board->epaper ? 2 : 16;
}

/* Floyd-Steinberg to four grays, in place: each pixel becomes black, 85, 170
 * or white, so a panel that rounds every pixel to its nearest gray (the
 * Sticky's) shows shading instead of bands. err holds 2 * (w + 2) values. */
static void dither_gray4_565(uint16_t *px, int w, int h, int16_t *err)
{
    static const uint16_t grays[4] = { 0x0000, 0x52AA, 0xAD55, 0xFFFF };
    int16_t *cur = err, *next = err + w + 2;
    memset(err, 0, 2 * (size_t)(w + 2) * sizeof(*err));
    for (int y = 0; y < h; y++) {
        memset(next, 0, (size_t)(w + 2) * sizeof(*next));
        for (int x = 0; x < w; x++) {
            uint16_t p = px[(size_t)y * w + x];
            int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            int v = ((77 * (r * 255 / 31) + 150 * (g * 255 / 63) + 29 * (b * 255 / 31)) >> 8) + cur[x + 1];
            int level = (v * 3 + 127) / 255;
            level = level < 0 ? 0 : level > 3 ? 3 : level;
            int e = v - level * 85;
            cur[x + 2] += (int16_t)(e * 7 / 16);
            next[x] += (int16_t)(e * 3 / 16);
            next[x + 1] += (int16_t)(e * 5 / 16);
            next[x + 2] += (int16_t)(e / 16);
            px[(size_t)y * w + x] = grays[level];
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
}

void muse_ui_image_done(void)
{
    if (!s_ready || muse_ui_image_bits() != 2) {
        return;   /* a colour panel shows each rectangle as it comes */
    }
    int16_t *err = malloc(2 * (size_t)(s_w + 2) * sizeof(int16_t));
    if (!err) {
        return;   /* shown undithered: banded, but whole */
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        dither_gray4_565(s_image_buf, s_w, s_h, err);
        s_image_area = (lv_area_t){ 0, 0, s_w - 1, s_h - 1 };
        s_image_dirty = true;
    }
    xSemaphoreGive(s_image_mutex);
    free(err);
}

bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s_w || y + h > s_h) {
        return false;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_buf) {
        /* The first rectangle of an image: a black screen to draw onto. */
        s_image_buf = heap_caps_calloc((size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_image_buf) {
            xSemaphoreGive(s_image_mutex);
            return false;
        }
        /* Now, not when it's shown: asleep, the display may be paused (muse_input.c). */
        muse_state_set_asleep(false);
    }
    const uint8_t *src = (const uint8_t *)pixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_image_buf + (size_t)(y + row) * s_w + x;
        for (int i = 0; i < w; i++, src += 2) {
            dst[i] = (uint16_t)(src[0] << 8 | src[1]);
        }
    }
    lv_area_t *a = &s_image_area;
    if (!s_image_dirty) {
        *a = (lv_area_t){ x, y, x + w - 1, y + h - 1 };
        s_image_dirty = true;
    } else {
        a->x1 = LV_MIN(a->x1, x);
        a->y1 = LV_MIN(a->y1, y);
        a->x2 = LV_MAX(a->x2, x + w - 1);
        a->y2 = LV_MAX(a->y2, y + h - 1);
    }
    xSemaphoreGive(s_image_mutex);
    return true;
}

void muse_ui_image_hide(void)
{
    if (!s_ready) {
        return;
    }
    muse_board->display_lock(-1);
    image_hide_locked();
#if CONFIG_MUSE_WATCHER_CAMERA
    if (s_camera_hint) lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
#endif
    muse_board->display_unlock();
}

#if CONFIG_MUSE_WATCHER_CAMERA
void muse_ui_camera_hint(bool visible)
{
    if (!s_ready || !s_camera_hint) return;
    muse_board->display_lock(-1);
    if (visible) lv_obj_remove_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}
#endif





