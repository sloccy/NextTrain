#include "ui.h"
#include <string.h>
#include <stdio.h>

#define ICON_RADIUS 3

void ui_draw_route_icon(GContext *ctx, GRect bounds, char letter, GColor bg_color) {
  graphics_context_set_fill_color(ctx, bg_color);
  graphics_fill_rect(ctx, bounds, ICON_RADIUS, GCornersAll);

  char text[2] = {letter, 0};
  // Nudge text up 1px — system font baselines render slightly low visually
  GRect text_bounds = GRect(bounds.origin.x, bounds.origin.y - 1,
                            bounds.size.w, bounds.size.h + 2);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, text,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     text_bounds,
                     GTextOverflowModeFill,
                     GTextAlignmentCenter,
                     NULL);
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
    graphics_context_set_text_color(ctx, GColorDarkGray);
    GRect more_bounds = GRect(x_offset, ICON_Y + 4, 14, 20);
    graphics_draw_text(ctx, "\xe2\x80\xa6", // UTF-8 ellipsis
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       more_bounds, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    x_offset += 16;
  }

  return x_offset;
}

const char *ui_status_label(ArrivalStatus status) {
  switch (status) {
    case ARRIVAL_LIVE:      return "Live";
    case ARRIVAL_SCHEDULED: return "Sched";
    case ARRIVAL_CANCELED:  return "Cancel";
    case ARRIVAL_SKIPPED:   return "Skip";
    case ARRIVAL_ADDED:     return "Added";
    default:                return "";
  }
}

GColor ui_gcolor_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return GColorFromRGB(r, g, b);
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
