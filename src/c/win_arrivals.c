#include "win_arrivals.h"
#include "win_home.h"
#include "win_rename.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>

#define ROW_HEIGHT 56
#define HEADER_HEIGHT 30
#define ICON_SIZE 24
#define REFRESH_MIN_SEC 30   // never poll faster than every 30 s
#define REFRESH_MAX_SEC 300  // cap stale far-future next_refresh at 5 min

// ─── State ────────────────────────────────────────────────────────────────────

static Window      *s_window;
static Layer       *s_header_layer;
static MenuLayer   *s_menu;
static ActionMenu  *s_action_menu;
static AppTimer    *s_refresh_timer;
static ArrivalsParams s_params;
static bool         s_waiting;       // true if no data yet for initial load
static bool         s_have_status;   // true if last update was a status, not arrivals
static CommStatus   s_last_status;
static int8_t       s_existing_fav_idx; // index of matching favorite, or -1
static uint8_t      s_new_fav_idx;      // index of just-added favorite

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

// ─── Header layer draw ────────────────────────────────────────────────────────

static void prv_draw_header(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char display_name[40];
  slug_to_display(s_params.station_slug, display_name, sizeof(display_name));
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, display_name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(0, 2, bounds.size.w, HEADER_HEIGHT - 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Divider line
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, bounds.size.h - 1),
                          GPoint(bounds.size.w, bounds.size.h - 1));
}

// ─── MenuLayer callbacks ──────────────────────────────────────────────────────

static const ArrivalCache *prv_cache(void) {
  return state_get_arrival_cache(s_params.query_index);
}

// Layout: 0..N-1 arrival rows (or 1 status row if N == 0), then optional
// "Add Favorite" at the end in search mode.
static uint8_t prv_arrival_count(void) {
  const ArrivalCache *c = prv_cache();
  return (c && c->valid) ? c->count : 0;
}

static uint16_t prv_add_fav_row(void) {
  uint8_t arrivals = prv_arrival_count();
  return arrivals == 0 ? 1 : arrivals; // status row occupies row 0 when empty
}

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  if (s_waiting) return 1;
  uint16_t base = prv_arrival_count();
  if (base == 0) base = 1;
  return base + 1; // always show Add/Remove row at bottom
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return ROW_HEIGHT;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  hi     = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_fill_color(ctx, hi ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  // Subtle card outline on unhighlighted data rows
  bool is_action_row = (idx->row == prv_add_fav_row());
  if (!hi && !s_waiting && !is_action_row) {
    GRect box = GRect(bounds.origin.x + 2, bounds.origin.y + 2,
                      bounds.size.w - 4, bounds.size.h - 4);
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_round_rect(ctx, box, 4);
  }

  if (s_waiting) {
    graphics_draw_text(ctx, "Loading\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  const ArrivalCache *cache = prv_cache();
  uint8_t arrivals = prv_arrival_count();

  // Action row (always last): "Remove Favorite" if already saved, else "Add Favorite"
  if (idx->row == prv_add_fav_row()) {
    const char *label = (s_existing_fav_idx >= 0)
                      ? "\xe2\x98\x85 Remove Favorite"
                      : "\xe2\x98\x85 Add Favorite";
    menu_cell_basic_draw(ctx, cell, label, NULL, NULL);
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

  // Route icon
  GColor color  = ui_gcolor_from_rgb(e->r, e->g, e->b);
  GRect icon_r  = GRect(8, (bounds.size.h - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE);
  ui_draw_route_icon(ctx, icon_r, e->route[0], color);

  // Time (large)
  int16_t text_x = 8 + ICON_SIZE + 8;
  GRect time_r   = GRect(text_x, 8, 70, 22);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, e->time,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     time_r, GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // Status label (small, colored)
  bool is_delayed   = (e->status == ARRIVAL_LIVE);
  bool is_canceled  = (e->status == ARRIVAL_CANCELED || e->status == ARRIVAL_SKIPPED);
  GColor label_color = is_canceled ? GColorRed
                     : is_delayed  ? GColorGreen
                                   : GColorDarkGray;
  graphics_context_set_text_color(ctx, label_color);
  GRect label_r = GRect(text_x, 30, 90, 16);
  graphics_draw_text(ctx, e->label,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     label_r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Headsign (right side)
  int16_t hs_x = text_x + 80;
  GRect hs_r   = GRect(hs_x, 12, bounds.size.w - hs_x - 4, 32);
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, e->headsign,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     hs_r, GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void prv_dismiss_to_home(void *ctx) {
  // Navigation depth in search mode: home → station picker → route picker → arrivals.
  // Remove the two pickers silently then animate arrivals off so it looks like
  // a clean return to home.
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
    // "Remove Favorite" — confirm before deleting
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

  // "Add Favorite" — parse routes then prompt to rename
  Favorite fav;
  memset(&fav, 0, sizeof(fav));
  strncpy(fav.station_slug, s_params.station_slug, sizeof(fav.station_slug) - 1);

  const char *p = s_params.routes;
  while (*p && fav.route_count < MAX_FAV_ROUTES) {
    const char *seg_end = p;
    while (*seg_end && *seg_end != ',') seg_end++;

    const char *colon = p;
    while (colon < seg_end && *colon != ':') colon++;

    if (colon > p && colon + 1 < seg_end) {
      size_t name_len = (size_t)(colon - p);
      if (name_len > sizeof(fav.routes[0].route) - 1)
        name_len = sizeof(fav.routes[0].route) - 1;
      memcpy(fav.routes[fav.route_count].route, p, name_len);
      fav.routes[fav.route_count].route[name_len] = '\0';
      fav.routes[fav.route_count].dir = *(colon + 1);
      fav.route_count++;
    }

    p = seg_end;
    if (*p == ',') p++;
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

  // If we have cached data already (from background prefetch), render immediately
  const ArrivalCache *cached = prv_cache();
  if (cached && cached->valid) {
    s_waiting = false;
    if (cached->next_refresh) prv_schedule_refresh(cached->next_refresh);
  }

  // Always fire a fresh fetch regardless
  prv_do_refresh();
}

static void prv_window_unload(Window *win) {
  if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
  comm_set_arrivals_callback(NULL);
  comm_set_status_callback(NULL);
  menu_layer_destroy(s_menu);   s_menu = NULL;
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
