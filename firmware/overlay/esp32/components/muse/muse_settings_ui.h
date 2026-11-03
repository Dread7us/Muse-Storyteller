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

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/*
 * The settings tile (swipe left from Muse): Wi-Fi, Hatch, Bluetooth, Sound,
 * Sleep and Power pages. Runs entirely in the LVGL task; hardware state is
 * polled from the owning modules.
 */

void muse_settings_ui_build(lv_obj_t *tile);
void muse_settings_ui_open_sound(void);
void muse_settings_ui_open_home(void);
void muse_settings_ui_open_weather(void);
void muse_settings_ui_open_recorder(void);
void muse_settings_ui_bench_recorder_reader(bool original);
void muse_settings_ui_bench_recorder_status(void);
void muse_settings_ui_bench_recorder_sleep(int change);
bool muse_settings_ui_recorder_keep_awake(void);
/* LVGL task only. False when already at the boundary. */
bool muse_settings_ui_navigation(int direction);

/* Call periodically from the LVGL task; `visible` = settings tile is showing. */
void muse_settings_ui_tick(bool visible);

/* True when a sub-page is open (the tileview must not steal horizontal swipes). */
bool muse_settings_ui_in_subpage(void);
#if LV_USE_SNAPSHOT
void muse_settings_ui_bench_weather_forecast(void);
void muse_settings_ui_bench_sound(void);
void muse_settings_ui_bench_volume(int pct,bool release);
void muse_settings_ui_bench_speed(void);
#endif

void muse_settings_ui_transport_visible(bool visible);
