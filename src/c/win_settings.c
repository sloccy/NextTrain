#include "win_settings.h"
#include "win_edit_favorites.h"
#include "win_home.h"
#include "comm.h"
#include "state.h"
#include "ui.h"

#define REFRESH_WATCHDOG_MS 8000

static Window    *s_window;
static Layer     *s_header_layer;
static MenuLayer *s_menu;
static bool       s_refreshing;
static AppTimer  *s_watchdog;

static void prv_draw_header_layer(Layer *layer, GContext *ctx) {
  ui_draw_screen_header(ctx, layer_get_bounds(layer), "Settings", false, NULL);
}

static void prv_cancel_watchdog(const char *why) {
  if (s_watchdog) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "[settings] watchdog cancelled (%s)", why);
    app_timer_cancel(s_watchdog);
    s_watchdog = NULL;
  }
}

static void prv_watchdog_fire(void *ctx) {
  s_watchdog = NULL;
  if (!s_refreshing) return;
  APP_LOG(APP_LOG_LEVEL_WARNING,
          "[settings] watchdog: no JS response in %dms — phone JS may be down or worker unreachable",
          REFRESH_WATCHDOG_MS);
  s_refreshing = false;
  comm_set_status_callback(NULL);
  comm_set_stations_ready_callback(NULL);
  if (s_menu) menu_layer_reload_data(s_menu);
}

// Row indices
#define ROW_SHOW_RECENT  0
#define ROW_REFRESH      1
#define ROW_EDIT_FAVS    2

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) { return 3; }
static int16_t  prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 44; }

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);

  if (idx->row == ROW_SHOW_RECENT) {
    menu_cell_basic_draw(ctx, cell, "Show Last Search", NULL, NULL);
    // Checkbox on the right edge
    bool on = state_get_show_recent();
    GRect cb = GRect(bounds.size.w - 26, (bounds.size.h - 22) / 2, 22, 22);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_rect(ctx, cb);
    if (on) {
      graphics_draw_line(ctx, GPoint(cb.origin.x + 3,  cb.origin.y + 11),
                               GPoint(cb.origin.x + 9,  cb.origin.y + 17));
      graphics_draw_line(ctx, GPoint(cb.origin.x + 9,  cb.origin.y + 17),
                               GPoint(cb.origin.x + 19, cb.origin.y + 5));
    }
  } else if (idx->row == ROW_REFRESH) {
    const char *label = s_refreshing ? "Refreshing\xe2\x80\xa6" : "Refresh Data";
    menu_cell_basic_draw(ctx, cell, label, NULL, NULL);
  } else {
    menu_cell_basic_draw(ctx, cell, "Edit Favorites", NULL, NULL);
  }
}

static void prv_status(uint8_t qi, CommStatus status) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[settings] refresh status callback: status=%d", (int)status);
  prv_cancel_watchdog("status received");
  s_refreshing = false;
  comm_set_status_callback(NULL);
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_stations_ready(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "[settings] stations_ready callback fired");
  prv_cancel_watchdog("stations ready");
  s_refreshing = false;
  comm_set_stations_ready_callback(NULL);
  comm_set_status_callback(NULL);
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->row == ROW_SHOW_RECENT) {
    state_set_show_recent(!state_get_show_recent());
    if (s_menu) menu_layer_reload_data(s_menu);
    win_home_reload();
  } else if (idx->row == ROW_REFRESH && !s_refreshing) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[settings] refresh requested, arming %dms watchdog", REFRESH_WATCHDOG_MS);
    s_refreshing = true;
    menu_layer_reload_data(s_menu);
    comm_set_stations_ready_callback(prv_stations_ready);
    comm_set_status_callback(prv_status);
    comm_request_refresh_stations();
    s_watchdog = app_timer_register(REFRESH_WATCHDOG_MS, prv_watchdog_fire, NULL);
  } else if (idx->row == ROW_EDIT_FAVS) {
    win_edit_favorites_push();
  }
}

static void prv_window_load(Window *win) {
  Layer *root  = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, NT_HEADER_H));
  layer_set_update_proc(s_header_layer, prv_draw_header_layer);
  layer_add_child(root, s_header_layer);

  GRect menu_bounds = GRect(0, NT_HEADER_H, bounds.size.w, bounds.size.h - NT_HEADER_H);
  s_menu = menu_layer_create(menu_bounds);
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
}

static void prv_window_unload(Window *win) {
  prv_cancel_watchdog("window unload");
  comm_set_stations_ready_callback(NULL);
  comm_set_status_callback(NULL);
  menu_layer_destroy(s_menu); s_menu = NULL;
  layer_destroy(s_header_layer); s_header_layer = NULL;
  s_refreshing = false;
  window_destroy(win);
  s_window = NULL;
}

void win_settings_push(void) {
  s_refreshing = false;
  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
