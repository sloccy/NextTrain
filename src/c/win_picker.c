#include "win_picker.h"
#include "win_arrivals.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>

// ─── Station Picker ───────────────────────────────────────────────────────────

#define STA_WATCHDOG_MS 6000

static Window    *s_sta_window;
static MenuLayer *s_sta_menu;
static bool       s_sta_error;
static AppTimer  *s_sta_watchdog;

static void prv_sta_stations_ready(void);
static void prv_sta_status(uint8_t qi, CommStatus status);

static void prv_sta_cancel_watchdog(const char *why) {
  if (s_sta_watchdog) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "[picker] watchdog cancelled (%s)", why);
    app_timer_cancel(s_sta_watchdog);
    s_sta_watchdog = NULL;
  }
}

static void prv_sta_watchdog_fire(void *ctx) {
  s_sta_watchdog = NULL;
  StationsCache *stations = state_get_stations();
  if (stations && stations->valid && stations->is_full) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[picker] watchdog fired but stations valid, ignoring");
    return;
  }
  APP_LOG(APP_LOG_LEVEL_WARNING,
          "[picker] watchdog: no JS response in %dms — phone JS may be down or worker unreachable",
          STA_WATCHDOG_MS);
  s_sta_error = true;
  if (s_sta_menu) menu_layer_reload_data(s_sta_menu);
}

static void prv_sta_arm_watchdog(void) {
  prv_sta_cancel_watchdog("re-arm");
  APP_LOG(APP_LOG_LEVEL_INFO, "[picker] arming %dms watchdog for stations response", STA_WATCHDOG_MS);
  s_sta_watchdog = app_timer_register(STA_WATCHDOG_MS, prv_sta_watchdog_fire, NULL);
}

static uint16_t prv_sta_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  if (s_sta_error) return 1;
  StationsCache *stations = state_get_stations();
  if (!stations || !stations->valid || !stations->is_full) return 1; // loading row
  return stations->station_count;
}

static int16_t prv_sta_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 44; }

static void prv_sta_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  if (s_sta_error) {
    graphics_draw_text(ctx, "Offline \xe2\x80\x93 select to retry",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       layer_get_bounds(cell),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }
  StationsCache *stations = state_get_stations();
  if (!stations || !stations->valid || !stations->is_full) {
    graphics_draw_text(ctx, "Loading stations\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       layer_get_bounds(cell),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }
  if (idx->row >= stations->station_count) return;
  char display_name[40];
  slug_to_display(stations->stations[idx->row].slug, display_name, sizeof(display_name));
  menu_cell_basic_draw(ctx, cell, display_name, NULL, NULL);
}

static void prv_sta_status(uint8_t qi, CommStatus status) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[picker] sta_status fired: status=%d qi=%d", (int)status, (int)qi);
  prv_sta_cancel_watchdog("status received");
  StationsCache *stations = state_get_stations();
  if (stations && stations->valid && stations->is_full) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[picker] sta_status: stations already valid, ignoring");
    return;
  }
  s_sta_error = true;
  if (s_sta_menu) menu_layer_reload_data(s_sta_menu);
}

static void prv_sta_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_sta_error) {
    s_sta_error = false;
    comm_set_stations_ready_callback(prv_sta_stations_ready);
    comm_set_status_callback(prv_sta_status);
    comm_request_stations_full();
    prv_sta_arm_watchdog();
    if (s_sta_menu) menu_layer_reload_data(s_sta_menu);
    return;
  }
  StationsCache *stations = state_get_stations();
  if (!stations || !stations->valid) return;
  if (idx->row >= stations->station_count) return;
  const Station *st = &stations->stations[idx->row];
  win_route_picker_push(st->slug);
}

static void prv_sta_stations_ready(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "[picker] sta_stations_ready fired");
  StationsCache *stations = state_get_stations();
  if (!stations || !stations->is_full) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[picker] sta_stations_ready: subset only, waiting for full sync");
    return;
  }
  prv_sta_cancel_watchdog("stations ready");
  s_sta_error = false;
  if (s_sta_menu) menu_layer_reload_data(s_sta_menu);
}

static void prv_sta_window_load(Window *win) {
  StationsCache *existing = state_get_stations();
  APP_LOG(APP_LOG_LEVEL_INFO, "[picker] sta_window_load: stations_valid=%s",
          (existing && existing->valid) ? "YES" : "NO");
  Layer *root = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  s_sta_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_sta_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows  = prv_sta_num_rows,
    .get_cell_height = prv_sta_row_height,
    .draw_row      = prv_sta_draw_row,
    .select_click  = prv_sta_select,
  });
  menu_layer_set_normal_colors(s_sta_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_sta_menu, GColorLightGray, GColorBlack);
  menu_layer_set_click_config_onto_window(s_sta_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_sta_menu));

  comm_set_stations_ready_callback(prv_sta_stations_ready);
  comm_set_status_callback(prv_sta_status);

  // Need the full station list for search. Request it if we don't have it yet
  // (the persist-loaded subset is for home-screen icon rendering only).
  if (!existing || !existing->valid || !existing->is_full) {
    comm_request_stations_full();
    prv_sta_arm_watchdog();
  }
}

static void prv_sta_window_unload(Window *win) {
  prv_sta_cancel_watchdog("window unload");
  comm_set_stations_ready_callback(NULL);
  comm_set_status_callback(NULL);
  menu_layer_destroy(s_sta_menu);
  s_sta_menu = NULL;
  s_sta_error = false;
  window_destroy(win);
  s_sta_window = NULL;
}

void win_station_picker_push(void) {
  s_sta_window = window_create();
  window_set_background_color(s_sta_window, GColorWhite);
  window_set_window_handlers(s_sta_window, (WindowHandlers){
    .load   = prv_sta_window_load,
    .unload = prv_sta_window_unload,
  });
  window_stack_push(s_sta_window, true);
}

// ─── Route Picker ─────────────────────────────────────────────────────────────

// Transient picker state (lives for the duration of this window)
typedef struct {
  char slug[40];
} RoutePickerCtx;

static Window       *s_rte_window;
static MenuLayer    *s_rte_menu;
static RoutePickerCtx *s_rte_ctx;
static bool          s_selected[MAX_ROUTES_PER_STATION]; // which routes are checked

#define SECTION_ROUTES  0
#define SECTION_CONFIRM 1

static const Station *prv_station(void) {
  if (!s_rte_ctx) return NULL;
  return state_find_station(s_rte_ctx->slug);
}

static uint16_t prv_rte_num_sections(MenuLayer *ml, void *ctx) { return 2; }

static uint16_t prv_rte_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  if (s == SECTION_ROUTES) {
    const Station *st = prv_station();
    return st ? st->route_count : 0;
  }
  return 1; // "Show arrivals"
}

static int16_t prv_rte_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return (idx->section == SECTION_ROUTES) ? 52 : 40;
}

static int16_t prv_rte_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return (section == SECTION_CONFIRM) ? MENU_CELL_BASIC_HEADER_HEIGHT : 0;
}

static void prv_rte_draw_header(GContext *ctx, const Layer *cell, uint16_t s, void *c) {
  if (s == SECTION_CONFIRM) menu_cell_basic_header_draw(ctx, cell, "");
}

static void prv_rte_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  hi     = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_fill_color(ctx, hi ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (idx->section == SECTION_CONFIRM) {
    menu_cell_basic_draw(ctx, cell, "Show arrivals", NULL, NULL);
    return;
  }

  const Station *st = prv_station();
  if (!st || idx->row >= st->route_count) return;
  const StationRoute *rt = &st->routes[idx->row];

  // Checkbox indicator (right edge)
  GRect cb = GRect(bounds.size.w - 26, (bounds.size.h - 22) / 2, 22, 22);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, cb);
  if (s_selected[idx->row]) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(cb.origin.x + 3, cb.origin.y + 11),
                             GPoint(cb.origin.x + 9, cb.origin.y + 17));
    graphics_draw_line(ctx, GPoint(cb.origin.x + 9, cb.origin.y + 17),
                             GPoint(cb.origin.x + 19, cb.origin.y + 5));
  }

  // Route icon
  GColor color = ui_gcolor_from_rgb(rt->r, rt->g, rt->b);
  GRect icon = GRect(8, (bounds.size.h - 24) / 2, 24, 24);
  ui_draw_route_icon(ctx, icon, rt->route[0], color);

  // Headsign + direction
  graphics_context_set_text_color(ctx, GColorBlack);
  char label[36];
  snprintf(label, sizeof(label), "%s", rt->headsign);
  GRect text = GRect(40, (bounds.size.h - 18) / 2, bounds.size.w - 72, 20);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     text, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char dir_label[4];
  snprintf(dir_label, sizeof(dir_label), "(%c)", rt->dir);
  GRect dir_rect = GRect(text.origin.x, text.origin.y + 18, text.size.w, 14);
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, dir_label, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     dir_rect, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void prv_rte_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->section == SECTION_ROUTES) {
    s_selected[idx->row] = !s_selected[idx->row];
    menu_layer_reload_data(s_rte_menu);
    return;
  }

  // "Show arrivals" tapped — build routes string from selected
  const Station *st = prv_station();
  if (!st) return;

  char routes[64] = {0};
  bool any = false;
  for (uint8_t i = 0; i < st->route_count && i < MAX_ROUTES_PER_STATION; i++) {
    if (!s_selected[i]) continue;
    char part[8];
    snprintf(part, sizeof(part), "%s:%c", st->routes[i].route, st->routes[i].dir);
    if (any) strncat(routes, ",", sizeof(routes) - strlen(routes) - 1);
    strncat(routes, part, sizeof(routes) - strlen(routes) - 1);
    any = true;
  }
  if (!any) return; // nothing selected — ignore

  ArrivalsParams params;
  memset(&params, 0, sizeof(params));
  strncpy(params.station_slug, s_rte_ctx->slug, sizeof(params.station_slug) - 1);
  strncpy(params.routes,       routes,          sizeof(params.routes) - 1);
  params.query_index   = QUERY_INDEX_TRANSIENT;
  params.from_favorite = false;
  win_arrivals_push(&params);
}

static void prv_rte_window_load(Window *win) {
  Layer *root  = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);
  memset(s_selected, 0, sizeof(s_selected));

  s_rte_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_rte_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections  = prv_rte_num_sections,
    .get_num_rows      = prv_rte_num_rows,
    .get_cell_height   = prv_rte_row_height,
    .get_header_height = prv_rte_header_height,
    .draw_header       = prv_rte_draw_header,
    .draw_row          = prv_rte_draw_row,
    .select_click      = prv_rte_select,
  });
  menu_layer_set_normal_colors(s_rte_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_rte_menu, GColorLightGray, GColorBlack);
  menu_layer_set_click_config_onto_window(s_rte_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_rte_menu));
}

static void prv_rte_window_unload(Window *win) {
  menu_layer_destroy(s_rte_menu);
  s_rte_menu = NULL;
  if (s_rte_ctx) { free(s_rte_ctx); s_rte_ctx = NULL; }
  window_destroy(win);
  s_rte_window = NULL;
}

void win_route_picker_push(const char *station_slug) {
  s_rte_ctx = malloc(sizeof(RoutePickerCtx));
  if (!s_rte_ctx) return;
  strncpy(s_rte_ctx->slug, station_slug, sizeof(s_rte_ctx->slug) - 1);

  s_rte_window = window_create();
  window_set_background_color(s_rte_window, GColorWhite);
  window_set_window_handlers(s_rte_window, (WindowHandlers){
    .load   = prv_rte_window_load,
    .unload = prv_rte_window_unload,
  });
  window_stack_push(s_rte_window, true);
}
