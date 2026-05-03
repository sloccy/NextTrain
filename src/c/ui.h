#pragma once

#include <pebble.h>
#include "state.h"

// Draw a route icon: rounded square in bg_color, white bold letter centered.
// bounds: the target GRect (typically 24×24 or 26×26).
void ui_draw_route_icon(GContext *ctx, GRect bounds, char letter, GColor bg_color);

// Draw a row of route icons for a Favorite, starting at x_offset within bounds.
// Returns the x position after the last icon.
int16_t ui_draw_route_icons(GContext *ctx, GRect bounds, const Favorite *fav,
                             const StationsCache *stations, int16_t x_offset);

// Map ArrivalStatus to a short display string.
const char *ui_status_label(ArrivalStatus status);

// Convert raw r,g,b bytes to the nearest Pebble GColor.
GColor ui_gcolor_from_rgb(uint8_t r, uint8_t g, uint8_t b);

// Format a Favorite's routes as "A→E, B→N" into buf.
void ui_format_routes(const Favorite *fav, char *buf, size_t buf_size);
