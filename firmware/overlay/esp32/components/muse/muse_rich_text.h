/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MUSE_RICH_RAW_CAP 32768
#define MUSE_RICH_TEXT_CAP (MUSE_RICH_RAW_CAP * 3 + 1)
/* Style glyphs occupy one Unicode character each, preserving selection geometry. */
#define MUSE_RICH_BOLD_BASE 0xe100
#define MUSE_RICH_STYLE_CAP (MUSE_RICH_RAW_CAP * 2)
enum { MUSE_RICH_BOLD=1,MUSE_RICH_ITALIC=2,MUSE_RICH_STRIKE=4,MUSE_RICH_CODE=8,MUSE_RICH_LINK=16,MUSE_RICH_QUOTE=32 };
typedef struct {uint16_t start,end;char url[384];} muse_rich_link_t;
typedef struct {uint8_t *styles;size_t cap;unsigned count,link_count;uint8_t initial_style;muse_rich_link_t links[16];} muse_rich_meta_t;
bool muse_rich_render(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap);
bool muse_rich_render_style(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap,bool bold,bool code);
bool muse_rich_render_full(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap,bool bold,bool code,muse_rich_meta_t *meta);
