#include "ui.h"
#include <string.h>
#include <stdio.h>

#define ICON_RADIUS 3

// Black text with a 2 px white halo for legibility over any background hue.
// Stamps the text 24 times offset within a 5×5 grid (excluding origin), then
// the final black pass on top. Two 24-pt+ icons per screen → cheap enough.
static void prv_draw_text_haloed(GContext *ctx, const char *text, GFont font,
                                  GRect bounds) {
  graphics_context_set_text_color(ctx, GColorWhite);
  for (int8_t dx = -1; dx <= 2; dx++) {
    for (int8_t dy = -1; dy <= 1; dy++) {
      if ((dx == 0 || dx == 1) && dy == 0) continue;
      GRect off = GRect(bounds.origin.x + dx, bounds.origin.y + dy,
                        bounds.size.w, bounds.size.h);
      graphics_draw_text(ctx, text, font, off,
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    }
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, text, font, bounds,
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  GRect bold = GRect(bounds.origin.x + 1, bounds.origin.y,
                     bounds.size.w, bounds.size.h);
  graphics_draw_text(ctx, text, font, bold,
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

void ui_draw_route_icon(GContext *ctx, GRect bounds, char letter, GColor bg_color) {
  graphics_context_set_fill_color(ctx, bg_color);
  graphics_fill_rect(ctx, bounds, ICON_RADIUS, GCornersAll);

  char text[2] = {letter, 0};
  GRect text_bounds = GRect(bounds.origin.x, bounds.origin.y - 1,
                            bounds.size.w, bounds.size.h + 2);
  prv_draw_text_haloed(ctx, text,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       text_bounds);
}

int16_t ui_draw_route_icons(GContext *ctx, GRect bounds, const Favorite *fav,
                             const StationsCache *stations, int16_t x_offset) {
  if (!fav) return x_offset;

  const int16_t ICON_SIZE = 24;
  const int16_t ICON_GAP  = 3;
  const int16_t ICON_Y    = bounds.origin.y + (bounds.size.h - ICON_SIZE) / 2;
  const int     MAX_ICONS = 4;

  // Locate the station once — O(S) — rather than once per icon
  const Station *st = NULL;
  if (stations && stations->valid) {
    for (uint16_t s = 0; s < stations->station_count; s++) {
      if (strcmp(stations->stations[s].slug, fav->station_slug) == 0) {
        st = &stations->stations[s];
        break;
      }
    }
  }

  int drawn = 0;
  for (uint8_t i = 0; i < fav->route_count && drawn < MAX_ICONS; i++) {
    GColor color = GColorLightGray; // fallback if stations not loaded

    if (st) {
      for (uint8_t r = 0; r < st->route_count; r++) {
        if (st->routes[r].route[0] == fav->routes[i].route[0]
            && st->routes[r].dir == fav->routes[i].dir) {
          color = ui_gcolor_from_rgb(st->routes[r].r, st->routes[r].g, st->routes[r].b);
          break;
        }
      }
    }

    GRect icon_bounds = GRect(x_offset, ICON_Y, ICON_SIZE, ICON_SIZE);
    ui_draw_route_icon(ctx, icon_bounds, fav->routes[i].route[0], color);
    x_offset += ICON_SIZE + ICON_GAP;
    drawn++;
  }

  // If there are more routes than MAX_ICONS, show "…"
  if (fav->route_count > MAX_ICONS) {
    graphics_context_set_text_color(ctx, GColorBlack);
    GRect more_bounds = GRect(x_offset, ICON_Y + 4, 14, 20);
    graphics_draw_text(ctx, "\xe2\x80\xa6", // UTF-8 ellipsis
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       more_bounds, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    x_offset += 16;
  }

  return x_offset;
}

// ─── Composite favorite icon ──────────────────────────────────────────────────

#define FAV_ARROW_MARGIN  4
#define FAV_SQUARE_SIZE  36
#define FAV_BBOX_SIZE    44  // FAV_SQUARE_SIZE + 2 * FAV_ARROW_MARGIN

int16_t ui_draw_favorite_icon(GContext *ctx, GPoint origin,
                               const Favorite *fav, const StationsCache *stations) {
  if (!fav || fav->route_count == 0) return origin.x + FAV_BBOX_SIZE;

  uint8_t n = fav->route_count;
  if (n > 4) n = 4;

  // Locate the station's route entries once to resolve colors
  const Station *st = NULL;
  if (stations && stations->valid) {
    for (uint16_t s = 0; s < stations->station_count; s++) {
      if (strcmp(stations->stations[s].slug, fav->station_slug) == 0) {
        st = &stations->stations[s];
        break;
      }
    }
  }

  // Resolve up to 4 route colors
  GColor route_color[4];
  for (uint8_t i = 0; i < n; i++) {
    route_color[i] = GColorLightGray;
    if (st) {
      for (uint8_t r = 0; r < st->route_count; r++) {
        if (st->routes[r].route[0] == fav->routes[i].route[0]
            && st->routes[r].dir == fav->routes[i].dir) {
          route_color[i] = ui_gcolor_from_rgb(st->routes[r].r,
                                              st->routes[r].g,
                                              st->routes[r].b);
          break;
        }
      }
    }
  }

  int16_t sx  = origin.x + FAV_ARROW_MARGIN;
  int16_t sy  = origin.y + FAV_ARROW_MARGIN;
  int16_t mid = FAV_SQUARE_SIZE / 2; // 18 — also used by cardinal arrows below
  GRect sq = GRect(sx, sy, FAV_SQUARE_SIZE, FAV_SQUARE_SIZE);

  if (n == 1) {
    // Solid colored fill + large black letter with 2 px white halo
    graphics_context_set_fill_color(ctx, route_color[0]);
    graphics_fill_rect(ctx, sq, 3, GCornersAll);

    char text[2] = { fav->routes[0].route[0], 0 };
    GRect slot = GRect(sx, sy - 1, FAV_SQUARE_SIZE, FAV_SQUARE_SIZE + 2);
    prv_draw_text_haloed(ctx, text,
                         fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), slot);
  } else {
    // Multi-route: white fill + 8-segment colored outline + black letters
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, sq, 0, GCornerNone);

    int16_t e = FAV_SQUARE_SIZE - 1; // 35 (last pixel)

    int8_t seg_route[8];
    if (n == 2) {
      seg_route[0] = 0; seg_route[1] = 0; // top halves → route 0
      seg_route[2] = 1; seg_route[3] = 1; // right halves → route 1
      seg_route[4] = 1; seg_route[5] = 1; // bottom halves → route 1
      seg_route[6] = 0; seg_route[7] = 0; // left halves → route 0
    } else if (n == 3) {
      seg_route[0] = 0; seg_route[1] = 1; seg_route[2] = 1; seg_route[3] = 2;
      seg_route[4] = 2; seg_route[5] = 2; seg_route[6] = 2; seg_route[7] = 0;
    } else { // n == 4
      seg_route[0] = 0; seg_route[1] = 1; seg_route[2] = 1; seg_route[3] = 3;
      seg_route[4] = 3; seg_route[5] = 2; seg_route[6] = 2; seg_route[7] = 0;
    }

    GPoint seg_pts[8][2] = {
      { GPoint(sx,       sy      ), GPoint(sx + mid, sy      ) },
      { GPoint(sx + mid, sy      ), GPoint(sx + e,   sy      ) },
      { GPoint(sx + e,   sy      ), GPoint(sx + e,   sy + mid) },
      { GPoint(sx + e,   sy + mid), GPoint(sx + e,   sy + e  ) },
      { GPoint(sx + e,   sy + e  ), GPoint(sx + mid, sy + e  ) },
      { GPoint(sx + mid, sy + e  ), GPoint(sx,       sy + e  ) },
      { GPoint(sx,       sy + e  ), GPoint(sx,       sy + mid) },
      { GPoint(sx,       sy + mid), GPoint(sx,       sy      ) },
    };

    graphics_context_set_stroke_width(ctx, 2);
    for (int i = 0; i < 8; i++) {
      graphics_context_set_stroke_color(ctx, route_color[seg_route[i]]);
      graphics_draw_line(ctx, seg_pts[i][0], seg_pts[i][1]);
    }
    graphics_context_set_stroke_width(ctx, 1);

    // Black letters in each quadrant slot
    graphics_context_set_text_color(ctx, GColorBlack);
    int16_t half = FAV_SQUARE_SIZE / 2; // 18
    int16_t qoff = 1;
    GFont letter_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    GRect slot[4] = {
      GRect(sx + qoff, sy + qoff - 4, half - qoff, half + 1), // TL
      GRect(sx + half, sy + qoff - 4, half - qoff, half + 1), // TR
      GRect(sx + qoff, sy + half - 4, half - qoff, half + 1), // BL
      GRect(sx + half, sy + half - 4, half - qoff, half + 1), // BR
    };

    if (n == 2) {
      char t0[2] = { fav->routes[0].route[0], 0 };
      char t1[2] = { fav->routes[1].route[0], 0 };
      graphics_draw_text(ctx, t0, letter_font, slot[0],
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, t1, letter_font, slot[3],
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    } else if (n == 3) {
      char t0[2] = { fav->routes[0].route[0], 0 };
      char t1[2] = { fav->routes[1].route[0], 0 };
      char t2[2] = { fav->routes[2].route[0], 0 };
      GRect bc = GRect(sx + half / 2, sy + half - 4, half, half + 1);
      graphics_draw_text(ctx, t0, letter_font, slot[0],
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, t1, letter_font, slot[1],
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, t2, letter_font, bc,
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    } else { // n == 4
      for (int i = 0; i < 4; i++) {
        char t[2] = { fav->routes[i].route[0], 0 };
        graphics_draw_text(ctx, t, letter_font, slot[i],
                           GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      }
    }
  }

  // Cardinal arrows: one per unique direction, outside the square.
  // Filled red with 1px black outline using GPath.
  bool dirs[4] = { false, false, false, false }; // N, S, E, W
  for (uint8_t i = 0; i < n; i++) {
    switch (fav->routes[i].dir) {
      case 'N': dirs[0] = true; break;
      case 'S': dirs[1] = true; break;
      case 'E': dirs[2] = true; break;
      case 'W': dirs[3] = true; break;
    }
  }

  int16_t cx = sx + mid; // center x of square
  int16_t cy = sy + mid; // center y of square
  int16_t by = sy + FAV_SQUARE_SIZE;
  int16_t rx = sx + FAV_SQUARE_SIZE;

  if (dirs[0]) { // N: pointing up
    GPoint pts[3] = { GPoint(cx, sy - 4), GPoint(cx - 3, sy + 1), GPoint(cx + 3, sy + 1) };
    GPathInfo info = { .num_points = 3, .points = pts };
    GPath *p = gpath_create(&info);
    if (p) {
      graphics_context_set_fill_color(ctx, GColorRed);
      gpath_draw_filled(ctx, p);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      gpath_draw_outline(ctx, p);
      gpath_destroy(p);
    }
  }
  if (dirs[1]) { // S: pointing down
    GPoint pts[3] = { GPoint(cx, by + 4), GPoint(cx - 3, by - 1), GPoint(cx + 3, by - 1) };
    GPathInfo info = { .num_points = 3, .points = pts };
    GPath *p = gpath_create(&info);
    if (p) {
      graphics_context_set_fill_color(ctx, GColorRed);
      gpath_draw_filled(ctx, p);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      gpath_draw_outline(ctx, p);
      gpath_destroy(p);
    }
  }
  if (dirs[2]) { // E: pointing right
    GPoint pts[3] = { GPoint(rx + 4, cy), GPoint(rx - 1, cy - 3), GPoint(rx - 1, cy + 3) };
    GPathInfo info = { .num_points = 3, .points = pts };
    GPath *p = gpath_create(&info);
    if (p) {
      graphics_context_set_fill_color(ctx, GColorRed);
      gpath_draw_filled(ctx, p);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      gpath_draw_outline(ctx, p);
      gpath_destroy(p);
    }
  }
  if (dirs[3]) { // W: pointing left
    GPoint pts[3] = { GPoint(sx - 4, cy), GPoint(sx + 1, cy - 3), GPoint(sx + 1, cy + 3) };
    GPathInfo info = { .num_points = 3, .points = pts };
    GPath *p = gpath_create(&info);
    if (p) {
      graphics_context_set_fill_color(ctx, GColorRed);
      gpath_draw_filled(ctx, p);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      gpath_draw_outline(ctx, p);
      gpath_destroy(p);
    }
  }

  // Restore context state
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  return origin.x + FAV_BBOX_SIZE;
}

GColor ui_gcolor_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return GColorFromRGB(r, g, b);
}

void ui_draw_screen_header(GContext *ctx, GRect bounds,
                            const char *title, bool star) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int16_t title_w = bounds.size.w - NT_PADDING_X - (star ? 22 : NT_PADDING_X);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(NT_PADDING_X, 2, title_w, bounds.size.h - 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (star) {
    graphics_draw_text(ctx, "\xe2\x98\x85",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(bounds.size.w - 22, (bounds.size.h - 16) / 2, 18, 16),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, bounds.size.h - 2, bounds.size.w, 2), 0, GCornerNone);
}

void ui_format_routes(const Favorite *fav, char *buf, size_t buf_size) {
  if (!fav || buf_size == 0) return;
  buf[0] = 0;
  for (uint8_t i = 0; i < fav->route_count; i++) {
    char part[8];
    snprintf(part, sizeof(part), "%s\xe2\x86\x92%c", // "A→N"
             fav->routes[i].route, fav->routes[i].dir);
    if (i > 0) strncat(buf, ", ", buf_size - strlen(buf) - 1);
    strncat(buf, part, buf_size - strlen(buf) - 1);
  }
}
