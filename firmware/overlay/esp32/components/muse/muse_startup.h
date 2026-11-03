#pragma once
#include <stdbool.h>
/* LVGL task only. preview ignores the saved boot preference, but never changes it. */
void muse_startup_show(bool preview);
void muse_startup_hide(void);
bool muse_startup_visible(void);
