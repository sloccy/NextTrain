#include "win_edit_favorites.h"
#include "win_home.h"
#include "win_rename.h"
#include "state.h"
#include "ui.h"
#include <string.h>

static Window       *s_window;
static Layer        *s_header_layer;
static MenuLayer    *s_menu;
static ActionMenu   *s_action_menu;
static uint8_t       s_selected_row;

static void prv_draw_header_layer(Layer *layer, GContext *ctx) {
  ui_draw_screen_header(ctx, layer_get_bounds(layer), "Edit Favorites", false);
}

// ─── ActionMenu ───────────────────────────────────────────────────────────────

enum { ACTION_MOVE_UP = 1, ACTION_MOVE_DOWN, ACTION_RENAME, ACTION_DELETE };

static void prv_rename_done(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
  win_home_reload();
}

static void prv_action_performed(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  uint32_t action = (uint32_t)(uintptr_t)action_menu_item_get_action_data(item);
  uint8_t  count  = state_get_favorite_count();

  switch (action) {
    case ACTION_MOVE_UP:
      if (s_selected_row > 0) {
        state_swap_favorites(s_selected_row, s_selected_row - 1);
        s_selected_row--;
      }
      break;
    case ACTION_MOVE_DOWN:
      if (s_selected_row + 1 < count) {
        state_swap_favorites(s_selected_row, s_selected_row + 1);
        s_selected_row++;
      }
      break;
    case ACTION_RENAME:
      win_rename_start(s_selected_row, prv_rename_done);
      break;
    case ACTION_DELETE:
      state_remove_favorite(s_selected_row);
      if (s_selected_row >= state_get_favorite_count() && s_selected_row > 0)
        s_selected_row--;
      win_home_reload();
      break;
  }
  menu_layer_reload_data(s_menu);
}

static void prv_show_action_menu(uint8_t row) {
  s_selected_row = row;
  uint8_t count  = state_get_favorite_count();

  // Only add Move Up / Move Down when the action is actually possible
  uint8_t capacity = 2 + (row > 0 ? 1 : 0) + (row + 1 < count ? 1 : 0); // +2 for Rename+Delete
  ActionMenuLevel *root = action_menu_level_create(capacity);
  if (row > 0)
    action_menu_level_add_action(root, "Move Up",   prv_action_performed,
                                 (void *)(uintptr_t)ACTION_MOVE_UP);
  if (row + 1 < count)
    action_menu_level_add_action(root, "Move Down", prv_action_performed,
                                 (void *)(uintptr_t)ACTION_MOVE_DOWN);
  action_menu_level_add_action(root, "Rename", prv_action_performed,
                               (void *)(uintptr_t)ACTION_RENAME);
  action_menu_level_add_action(root, "Delete", prv_action_performed,
                               (void *)(uintptr_t)ACTION_DELETE);

  ActionMenuConfig cfg = {
    .root_level = root,
    .colors     = { .background = GColorWhite, .foreground = GColorBlack },
    .align      = ActionMenuAlignTop,
  };
  s_action_menu = action_menu_open(&cfg);
}

// ─── MenuLayer callbacks ──────────────────────────────────────────────────────

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  uint8_t n = state_get_favorite_count();
  return n > 0 ? n : 1;
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 52; }

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  hi     = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_fill_color(ctx, hi ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (state_get_favorite_count() == 0) {
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, "No favorites to edit",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  Favorite *fav = state_get_favorite(idx->row);
  if (!fav) return;

  StationsCache *stations = state_get_stations();

  // Composite icon on the left
  GPoint icon_origin = GPoint(4, (bounds.size.h - 44) / 2);
  ui_draw_favorite_icon(ctx, icon_origin, fav, stations);

  // Reorder arrows indicator (right edge)
  graphics_context_set_text_color(ctx, GColorBlack);
  GRect arrows = GRect(bounds.size.w - 20, (bounds.size.h - 18) / 2, 18, 18);
  graphics_draw_text(ctx, "\xe2\x87\x85", // ⇅
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     arrows, GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Station name
  char display_name[40];
  if (fav->name[0]) {
    strncpy(display_name, fav->name, sizeof(display_name) - 1);
    display_name[sizeof(display_name) - 1] = 0;
  } else {
    slug_to_display(fav->station_slug, display_name, sizeof(display_name));
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  int16_t text_x = 4 + 44 + 4; // icon_margin + bbox_size + gap
  GRect name_r = GRect(text_x, 6, bounds.size.w - text_x - 24, 20);
  graphics_draw_text(ctx, display_name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     name_r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Routes subtitle
  char routes_str[40] = {0};
  ui_format_routes(fav, routes_str, sizeof(routes_str));
  graphics_context_set_text_color(ctx, GColorBlack);
  GRect sub_r = GRect(text_x, 28, bounds.size.w - text_x - 24, 16);
  graphics_draw_text(ctx, routes_str,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     sub_r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (state_get_favorite_count() == 0) return;
  prv_show_action_menu(idx->row);
}

// ─── Window lifecycle ─────────────────────────────────────────────────────────

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
  menu_layer_destroy(s_menu); s_menu = NULL;
  layer_destroy(s_header_layer); s_header_layer = NULL;
  window_destroy(win);
  s_window = NULL;
}

void win_edit_favorites_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
