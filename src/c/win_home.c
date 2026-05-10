#include "win_home.h"
#include "win_picker.h"
#include "win_arrivals.h"
#include "win_settings.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include "format.h"
#include <string.h>

// ─── Row geometry ─────────────────────────────────────────────────────────────

#define SECTION_FAVORITES 0
#define SECTION_ACTIONS   1

#define ROW_HEIGHT_FAV    NT_ROW_H_FAV     // 56
#define ROW_HEIGHT_ACTION NT_ROW_H_ACTION  // 40
#define ICON_SIZE         24
#define ICON_MARGIN_LEFT   8
#define TEXT_MARGIN_LEFT   8

// ─── State ────────────────────────────────────────────────────────────────────

static Window    *s_window;
static MenuLayer *s_menu;

static void prv_fav_renamed(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_arrivals_landed(uint8_t qi, const ArrivalCache *cache) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_tick(struct tm *tt, TimeUnits u) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_window_appear(Window *w) {
  comm_set_arrivals_callback(prv_arrivals_landed);
  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick);
}

static void prv_window_disappear(Window *w) {
  comm_set_arrivals_callback(NULL);
  tick_timer_service_unsubscribe();
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void prv_launch_favorite(uint8_t index) {
  Favorite *fav = state_get_favorite(index);
  if (!fav) return;

  char routes[64] = {0};
  state_format_routes_query(fav, routes, sizeof(routes));

  ArrivalsParams params;
  memset(&params, 0, sizeof(params));
  strncpy(params.station_slug, fav->station_slug, sizeof(params.station_slug) - 1);
  strncpy(params.routes,       routes,            sizeof(params.routes) - 1);
  params.query_index   = index;
  params.from_favorite = true;
  win_arrivals_push(&params);
}

// ─── MenuLayer callbacks ──────────────────────────────────────────────────────

static uint16_t prv_num_sections(MenuLayer *ml, void *ctx) {
  return 2;
}

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  if (section == SECTION_FAVORITES) {
    uint8_t n = state_get_favorite_count();
    return n > 0 ? n : 1; // 1 = empty-state placeholder
  }
  return 2; // "New Search", "Settings"
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return (idx->section == SECTION_FAVORITES) ? ROW_HEIGHT_FAV : ROW_HEIGHT_ACTION;
}

static int16_t prv_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return 0;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  highlight = menu_cell_layer_is_highlighted(cell);

  // White background always (even when highlighted we draw our own fill)
  graphics_context_set_fill_color(ctx, highlight ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (idx->section == SECTION_FAVORITES) {
    uint8_t count = state_get_favorite_count();
    if (count == 0) {
      // Empty-state
      graphics_draw_text(ctx, "No favorites yet \xe2\x80\x94 tap below",
                         fonts_get_system_font(FONT_KEY_GOTHIC_14),
                         GRect(8, (bounds.size.h - 16) / 2, bounds.size.w - 16, 20),
                         GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      return;
    }

    Favorite *fav = state_get_favorite(idx->row);
    if (!fav) return;

    StationsCache *stations = state_get_stations();

    // Draw composite favorite icon
    GPoint icon_origin = GPoint(ICON_MARGIN_LEFT,
                                bounds.origin.y + (bounds.size.h - 44) / 2);
    int16_t x = ui_draw_favorite_icon(ctx, icon_origin, fav, stations);
    graphics_context_set_text_color(ctx, GColorBlack);

    // Resolve display name
    x += TEXT_MARGIN_LEFT;
    char display_name[40];
    if (fav->name[0]) {
      strncpy(display_name, fav->name, sizeof(display_name) - 1);
      display_name[sizeof(display_name) - 1] = 0;
    } else {
      slug_to_display(fav->station_slug, display_name, sizeof(display_name));
    }

    // Position text: always two-line stack (name + subtitle) centered on icon.
    // Pebble fonts have extra top-leading so we nudge up to visually center.
    ArrivalCache *cache = state_get_arrival_cache(idx->row);

    const int16_t NAME_BOX_H  = 22;
    const int16_t SUB_BOX_H   = 16;
    const int16_t VISUAL_NUDGE = 4;
    int16_t icon_cy  = bounds.size.h / 2;
    int16_t text_w   = bounds.size.w - x - 4;
    int16_t stack_h  = NAME_BOX_H + SUB_BOX_H;
    int16_t name_top = icon_cy - stack_h / 2 - VISUAL_NUDGE;

    GRect name_bounds = GRect(x, name_top, text_w, NAME_BOX_H);
    graphics_draw_text(ctx, display_name,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       name_bounds, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    char sub[32];
    if (cache && cache->valid && cache->count > 0) {
      char wall[10], cd[10];
      uint16_t pred = format_arrival_predicted_min(cache->entries[0].mins,
                                                   cache->entries[0].st);
      format_wall_time(cache->entries[0].mins, wall, sizeof(wall));
      format_countdown(pred, cd, sizeof(cd));
      snprintf(sub, sizeof(sub), "Next: %s \xc2\xb7 %s", wall, cd);
    } else {
      strncpy(sub, "Loading\xe2\x80\xa6", sizeof(sub));
    }
    GRect sub_bounds = GRect(x, name_top + NAME_BOX_H, text_w, SUB_BOX_H);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       sub_bounds, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  // Action rows
  const char *labels[] = {"+ New Search", "Settings"};
  if (idx->row < 2) {
    menu_cell_basic_draw(ctx, cell, labels[idx->row], NULL, NULL);
  }
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->section == SECTION_FAVORITES) {
    if (state_get_favorite_count() == 0) { win_station_picker_push(); return; }
    prv_launch_favorite(idx->row);
    return;
  }
  if (idx->row == 0) win_station_picker_push();
  else               win_settings_push();
}

// ─── Window lifecycle ─────────────────────────────────────────────────────────

static void prv_window_load(Window *win) {
  Layer *root = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections  = prv_num_sections,
    .get_num_rows      = prv_num_rows,
    .get_cell_height   = prv_row_height,
    .get_header_height = prv_header_height,
    .draw_row          = prv_draw_row,
    .select_click      = prv_select,
  });
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, GColorLightGray, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_menu));
  comm_set_favorite_renamed_callback(prv_fav_renamed);
}

static void prv_window_unload(Window *win) {
  comm_set_favorite_renamed_callback(NULL);
  menu_layer_destroy(s_menu);
  s_menu = NULL;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void win_home_push(void) {
  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
      .load      = prv_window_load,
      .unload    = prv_window_unload,
      .appear    = prv_window_appear,
      .disappear = prv_window_disappear,
    });
    window_set_background_color(s_window, GColorWhite);
  }
  window_stack_push(s_window, true);
}

void win_home_reload(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
}
