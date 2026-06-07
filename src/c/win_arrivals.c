#include "win_arrivals.h"
#include "win_home.h"
#include "win_rename.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include "format.h"
#include <string.h>
#include <stdlib.h>

#define ROW_HEIGHT    NT_ROW_H_DATA   // 60
#define HEADER_HEIGHT NT_HEADER_H     // 32
#define ICON_SIZE     28
#define REFRESH_MIN_SEC 30
#define REFRESH_MAX_SEC 300

// Column layout (200 px screen)
// [4] [ICON 28] [4] [LEFT TEXT 116          ] [RIGHT TEXT 60] [4]
//  0   4        32   36..152                   136..196
#define COL_ICON_X    4
#define COL_LEFT_X   36   // COL_ICON_X + ICON_SIZE + 4
#define COL_LEFT_W   116
#define COL_RIGHT_X  136
#define COL_RIGHT_W  60
#define ROW_TOP_H    22   // headsign / wall-time box height
#define ROW_BOT_H    16   // status / countdown box height

// ─── State ────────────────────────────────────────────────────────────────────

static Window      *s_window;
static Layer       *s_header_layer;
static MenuLayer   *s_menu;
static ActionMenu  *s_action_menu;
static AppTimer    *s_refresh_timer;
static ArrivalsParams s_params;
static bool         s_waiting;
static bool         s_have_status;
static CommStatus   s_last_status;
static int8_t       s_existing_fav_idx;
static uint8_t      s_new_fav_idx;

// ─── Auto-refresh timer ───────────────────────────────────────────────────────

static void prv_do_refresh(void);

static void prv_timer_fired(void *ctx) {
  s_refresh_timer = NULL;
  prv_do_refresh();
}

static void prv_schedule_refresh(uint32_t next_refresh_unix) {
  if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
  uint32_t now_sec = (uint32_t)(time(NULL));
  uint32_t delay_sec = (next_refresh_unix > now_sec) ? (next_refresh_unix - now_sec) : 0;
  if (delay_sec < REFRESH_MIN_SEC) delay_sec = REFRESH_MIN_SEC;
  if (delay_sec > REFRESH_MAX_SEC) delay_sec = REFRESH_MAX_SEC;
  s_refresh_timer = app_timer_register(delay_sec * 1000, prv_timer_fired, NULL);
}

static void prv_do_refresh(void) {
  comm_request_arrivals(s_params.query_index, s_params.station_slug, s_params.routes);
}

// ─── Arrivals callback ────────────────────────────────────────────────────────

static void prv_arrivals_received(uint8_t query_index, const ArrivalCache *cache) {
  if (query_index != s_params.query_index) return;
  s_waiting     = false;
  s_have_status = false;
  if (s_menu) menu_layer_reload_data(s_menu);
  if (cache->next_refresh) prv_schedule_refresh(cache->next_refresh);
}

static void prv_status_received(uint8_t query_index, CommStatus status) {
  if (query_index != s_params.query_index) return;
  s_waiting     = false;
  s_have_status = true;
  s_last_status = status;
  if (s_menu) menu_layer_reload_data(s_menu);
}

// ─── Minute tick (live countdown) ────────────────────────────────────────────

static void prv_tick(struct tm *tt, TimeUnits u) {
  if (s_header_layer) layer_mark_dirty(s_header_layer);
  if (s_menu) layer_mark_dirty(menu_layer_get_layer(s_menu));
}

// ─── Header layer draw ────────────────────────────────────────────────────────

static void prv_draw_header(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  char display_name[40];
  slug_to_display(s_params.station_slug, display_name, sizeof(display_name));
  char time_buf[10];
  format_wall_time(format_now_minute_of_day(), time_buf, sizeof(time_buf));
  ui_draw_screen_header(ctx, bounds, display_name, s_params.from_favorite, time_buf);
}

// ─── MenuLayer callbacks ──────────────────────────────────────────────────────

static const ArrivalCache *prv_cache(void) {
  return state_get_arrival_cache(s_params.query_index);
}

static uint8_t prv_arrival_count(void) {
  const ArrivalCache *c = prv_cache();
  return (c && c->valid) ? c->count : 0;
}

static uint16_t prv_add_fav_row(void) {
  uint8_t arrivals = prv_arrival_count();
  return arrivals == 0 ? 1 : arrivals;
}

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  if (s_waiting) return 1;
  uint16_t base = prv_arrival_count();
  if (base == 0) base = 1;
  return base + 1;
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return ROW_HEIGHT;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  hi     = menu_cell_layer_is_highlighted(cell);

  graphics_context_set_fill_color(ctx, hi ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // 1 px black bottom rule (row divider)
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_line(ctx, GPoint(0, bounds.size.h - 1),
                          GPoint(bounds.size.w - 1, bounds.size.h - 1));

  graphics_context_set_text_color(ctx, GColorBlack);

  if (s_waiting) {
    graphics_draw_text(ctx, "Loading\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  const ArrivalCache *cache = prv_cache();
  uint8_t arrivals = prv_arrival_count();

  // Action row (always last): Add / Remove Favorite
  if (idx->row == prv_add_fav_row()) {
    const char *label = (s_existing_fav_idx >= 0)
                      ? "\xe2\x98\x85 Remove Favorite"
                      : "+ Add Favorite";
    graphics_draw_text(ctx, label,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(NT_PADDING_X, (bounds.size.h - 22) / 2,
                             bounds.size.w - NT_PADDING_X * 2, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  if (arrivals == 0) {
    const char *msg = "No upcoming trains";
    if (s_have_status) {
      switch (s_last_status) {
        case STATUS_OFFLINE: msg = "Offline";       break;
        case STATUS_ERROR:   msg = "Service error"; break;
        case STATUS_NO_DATA: msg = "No upcoming trains"; break;
        default:             break;
      }
    }
    graphics_draw_text(ctx, msg,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  if (idx->row >= arrivals) return;
  const ArrivalEntry *e = &cache->entries[idx->row];

  // ── Route badge ─────────────────────────────────────────────────────────────
  GColor color = ui_gcolor_from_rgb(e->r, e->g, e->b);
  GRect icon_r = GRect(COL_ICON_X, (bounds.size.h - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
  ui_draw_route_icon(ctx, icon_r, e->route[0], color);

  // Vertical centering geometry — same for left and right columns
  int16_t midline  = bounds.size.h / 2;
  int16_t stack_h  = ROW_TOP_H + ROW_BOT_H;
  int16_t top_y    = midline - stack_h / 2;
  int16_t bot_y    = top_y + ROW_TOP_H;

  // ── Left column: headsign (top) / status (bottom) ───────────────────────────
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, e->headsign,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(COL_LEFT_X, top_y, COL_LEFT_W, ROW_TOP_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char status_buf[12];
  GColor status_color;
  bool   status_bold;
  format_status_label(e->st, status_buf, sizeof(status_buf), &status_color, &status_bold);

  char status_line[64];
  strncpy(status_line, status_buf, sizeof(status_line) - 1);
  status_line[sizeof(status_line) - 1] = 0;

  graphics_context_set_text_color(ctx, status_color);
  graphics_draw_text(ctx, status_line,
                     fonts_get_system_font(status_bold
                                           ? FONT_KEY_GOTHIC_14_BOLD
                                           : FONT_KEY_GOTHIC_14),
                     GRect(COL_LEFT_X, bot_y, COL_LEFT_W, ROW_BOT_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // ── Right column: wall time (top) / countdown (bottom), right-aligned ───────
  char wall_buf[10];
  format_wall_time(e->mins, wall_buf, sizeof(wall_buf));

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, wall_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(COL_RIGHT_X, top_y, COL_RIGHT_W, ROW_TOP_H),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);

  char cd_buf[10];
  uint16_t pred = e->mins;
  format_countdown(pred, cd_buf, sizeof(cd_buf));

  graphics_draw_text(ctx, cd_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(COL_RIGHT_X, bot_y, COL_RIGHT_W, ROW_BOT_H),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void prv_dismiss_to_home(void *ctx) {
  window_stack_pop(false);  // arrivals
  window_stack_pop(false);  // route picker
  window_stack_pop(true);   // station picker → home
}

static void prv_rename_done(void) {
  win_home_reload();
  app_timer_register(0, prv_dismiss_to_home, NULL);
}

enum { ACTION_RENAME = 1, ACTION_KEEP_DEFAULT, ACTION_CONFIRM_DELETE };

static void prv_post_add_action(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  uint32_t action = (uint32_t)(uintptr_t)action_menu_item_get_action_data(item);
  if (action == ACTION_RENAME) {
    win_rename_start(s_new_fav_idx, prv_rename_done);
  } else {
    win_home_reload();
    app_timer_register(0, prv_dismiss_to_home, NULL);
  }
}

static void prv_delete_action(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  state_remove_favorite((uint8_t)s_existing_fav_idx);
  s_existing_fav_idx = -1;
  win_home_reload();
  app_timer_register(0, prv_dismiss_to_home, NULL);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->row != prv_add_fav_row()) return;

  if (s_existing_fav_idx >= 0) {
    ActionMenuLevel *root = action_menu_level_create(1);
    action_menu_level_add_action(root, "Confirm Delete", prv_delete_action, NULL);
    ActionMenuConfig cfg = {
      .root_level = root,
      .colors     = { .background = GColorWhite, .foreground = GColorBlack },
      .align      = ActionMenuAlignTop,
    };
    s_action_menu = action_menu_open(&cfg);
    return;
  }

  Favorite fav;
  memset(&fav, 0, sizeof(fav));
  strncpy(fav.station_slug, s_params.station_slug, sizeof(fav.station_slug) - 1);

  const char *p = s_params.routes;
  while (*p && *(p + 1) && fav.route_count < MAX_FAV_ROUTES) {
    fav.routes[fav.route_count].route[0] = *p;
    fav.routes[fav.route_count].route[1] = '\0';

    char dir_num = *(p + 1);
    char dir = 'N';
    if (dir_num == '1')      dir = 'S';
    else if (dir_num == '2') dir = 'E';
    else if (dir_num == '3') dir = 'W';

    fav.routes[fav.route_count].dir = dir;
    fav.route_count++;
    p += 2;
  }

  if (fav.route_count > 0) {
    state_add_favorite(&fav);
    s_new_fav_idx      = state_get_favorite_count() - 1;
    s_existing_fav_idx = (int8_t)s_new_fav_idx;
    win_home_reload();
    comm_request_arrivals(s_new_fav_idx, fav.station_slug, s_params.routes);

    ActionMenuLevel *root = action_menu_level_create(2);
    action_menu_level_add_action(root, "Rename", prv_post_add_action,
                                 (void *)(uintptr_t)ACTION_RENAME);
    action_menu_level_add_action(root, "Use default name", prv_post_add_action,
                                 (void *)(uintptr_t)ACTION_KEEP_DEFAULT);
    ActionMenuConfig cfg = {
      .root_level = root,
      .colors     = { .background = GColorWhite, .foreground = GColorBlack },
      .align      = ActionMenuAlignTop,
    };
    s_action_menu = action_menu_open(&cfg);
  }
}

// ─── Window lifecycle ─────────────────────────────────────────────────────────

static void prv_window_load(Window *win) {
  Layer *root  = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  // Header
  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
  layer_set_update_proc(s_header_layer, prv_draw_header);
  layer_add_child(root, s_header_layer);

  // Menu below header
  GRect menu_bounds = GRect(0, HEADER_HEIGHT, bounds.size.w, bounds.size.h - HEADER_HEIGHT);
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

  comm_set_arrivals_callback(prv_arrivals_received);
  comm_set_status_callback(prv_status_received);

  const ArrivalCache *cached = prv_cache();
  if (cached && cached->valid) {
    s_waiting = false;
    if (cached->next_refresh) prv_schedule_refresh(cached->next_refresh);
  }

  prv_do_refresh();
  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick);
}

static void prv_window_unload(Window *win) {
  tick_timer_service_unsubscribe();
  if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
  comm_set_arrivals_callback(NULL);
  comm_set_status_callback(NULL);
  menu_layer_destroy(s_menu);    s_menu = NULL;
  layer_destroy(s_header_layer); s_header_layer = NULL;
  window_destroy(win);
  s_window = NULL;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void win_arrivals_push(const ArrivalsParams *params) {
  s_params           = *params;
  s_waiting          = true;
  s_have_status      = false;
  s_existing_fav_idx = state_find_favorite_by_slug_and_routes(
                         params->station_slug, params->routes);

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
