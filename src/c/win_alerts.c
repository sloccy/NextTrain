#include "win_alerts.h"
#include "win_alert_detail.h"
#include "state.h"
#include "comm.h"
#include <string.h>
#include <stdio.h>

#define ROW_HEIGHT 44

static Window    *s_window;
static MenuLayer *s_menu;
static bool       s_waiting;

static void prv_reload(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_summary_received(const AlertSummaryCache *cache) {
  s_waiting = false;
  prv_reload();
}

// ─── MenuLayer callbacks ──────────────────────────────────────────────────────

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  if (s_waiting) return 1;
  const AlertSummaryCache *c = state_get_alert_summary();
  if (!c->valid || c->count == 0) return 1;
  return c->count;
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return ROW_HEIGHT;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  hi     = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_fill_color(ctx, hi ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_waiting) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Loading\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  const AlertSummaryCache *cache = state_get_alert_summary();
  if (!cache->valid || cache->count == 0) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "No active alerts",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  const AlertRouteSummary *r = &cache->routes[idx->row];
  char sub[20];
  snprintf(sub, sizeof(sub), "%u alert%s", (unsigned)r->count, r->count == 1 ? "" : "s");
  menu_cell_basic_draw(ctx, cell, r->name, sub, NULL);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  const AlertSummaryCache *cache = state_get_alert_summary();
  if (!cache->valid || cache->count == 0 || s_waiting) return;
  if (idx->row >= cache->count) return;
  win_alert_detail_push(cache->routes[idx->row].name);
}

// ─── Window lifecycle ─────────────────────────────────────────────────────────

static void prv_window_load(Window *win) {
  Layer *root  = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows    = prv_num_rows,
    .get_cell_height = prv_row_height,
    .draw_row        = prv_draw_row,
    .select_click    = prv_select,
  });
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, GColorLightGray, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  comm_set_alert_summary_callback(prv_summary_received);

  const AlertSummaryCache *cached = state_get_alert_summary();
  if (cached->valid) {
    s_waiting = false;
  } else {
    s_waiting = true;
    comm_request_alerts_summary();
  }
}

static void prv_window_unload(Window *win) {
  comm_set_alert_summary_callback(NULL);
  menu_layer_destroy(s_menu); s_menu = NULL;
  window_destroy(win);
  s_window = NULL;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void win_alerts_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
