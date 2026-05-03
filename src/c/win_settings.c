#include "win_settings.h"
#include "win_edit_favorites.h"
#include "comm.h"
#include "state.h"

static Window    *s_window;
static MenuLayer *s_menu;
static bool       s_refreshing;

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) { return 2; }
static int16_t  prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 44; }

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  if (idx->row == 0) {
    const char *label = s_refreshing ? "Refreshing\xe2\x80\xa6" : "Refresh Station Data";
    menu_cell_basic_draw(ctx, cell, label, NULL, NULL);
  } else {
    menu_cell_basic_draw(ctx, cell, "Edit Favorites", NULL, NULL);
  }
}

static void prv_status(uint8_t qi, CommStatus status) {
  s_refreshing = false;
  comm_set_status_callback(NULL);
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_stations_ready(void) {
  s_refreshing = false;
  comm_set_stations_ready_callback(NULL);
  comm_set_status_callback(NULL);
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->row == 0 && !s_refreshing) {
    s_refreshing = true;
    menu_layer_reload_data(s_menu);
    comm_set_stations_ready_callback(prv_stations_ready);
    comm_set_status_callback(prv_status);
    comm_request_refresh_stations();
  } else if (idx->row == 1) {
    win_edit_favorites_push();
  }
}

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
}

static void prv_window_unload(Window *win) {
  comm_set_stations_ready_callback(NULL);
  comm_set_status_callback(NULL);
  menu_layer_destroy(s_menu);
  s_menu = NULL;
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
