#pragma once

#include <pebble.h>
#include "state.h"

// ─── Shared layout constants ──────────────────────────────────────────────────

#define NT_HEADER_H      32
#define NT_ROW_H_DATA    60
#define NT_ROW_H_FAV     56
#define NT_ROW_H_PICKER  52
#define NT_ROW_H_ACTION  40
#define NT_PADDING_X      8

// Draw a route icon: rounded square in bg_color, white bold letter centered.
// bounds: the target GRect (typically 24×24 or 26×26).
void ui_draw_route_icon(GContext *ctx, GRect bounds, char letter, GColor bg_color);

// Draw a row of route icons for a Favorite, starting at x_offset within bounds.
// Returns the x position after the last icon.
int16_t ui_draw_route_icons(GContext *ctx, GRect bounds, const Favorite *fav,
                             const StationsCache *stations, int16_t x_offset);

// Composite favorite icon: white square with per-quadrant colored outline,
// black letters arranged by route count (1=centered, 2=TL+BR, 3=TL+TR+BC,
// 4=corners), and dedup'd cardinal arrows outside the square.
// origin is the top-left of the 44×44 bounding box (36px square + 4px margins).
// Returns the right edge x coordinate.
int16_t ui_draw_favorite_icon(GContext *ctx, GPoint origin, const Favorite *fav,
                              const StationsCache *stations);

// Convert raw r,g,b bytes to the nearest Pebble GColor.
GColor ui_gcolor_from_rgb(uint8_t r, uint8_t g, uint8_t b);

// Format a Favorite's routes as "A→E, B→N" into buf.
void ui_format_routes(const Favorite *fav, char *buf, size_t buf_size);

// Draw the editorial screen header: white fill, title left-aligned in
// GOTHIC_18_BOLD, optional ★ glyph right-aligned, 2 px solid black bottom rule.
// time_str: if non-NULL, shown right-aligned in GOTHIC_14_BOLD (e.g. "3:45p").
void ui_draw_screen_header(GContext *ctx, GRect bounds,
                            const char *title, bool star,
                            const char *time_str);
