#include "win_arrivals.h"
#include "win_home.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>

#define ROW_HEIGHT 56
#define HEADER_HEIGHT 30
#define ICON_SIZE 24

// ─── State ────────────────────────────────────────────────────────────────────

static Window      *s_window;
static Layer       *s_header_layer;
static MenuLayer   *s_menu;
static AppTimer    *s_refresh_timer;
static ArrivalsParams s_params;
static bool         s_waiting;   // true if no data yet for initial load

// ─── Auto-refresh timer ───────────────────────────────────────────────────────

static void prv_do_refresh(void);

static void prv_timer_fired(void *ctx) {
  s_refresh_timer = NULL;
  prv_do_refresh();
}

static void prv_schedule_refresh(uint32_t next_refresh_unix) {
  if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
  uint32_t now_sec = (uint32_t)(time(NULL));
  if (next_refresh_unix <= now_sec) {
    prv_do_refresh();
    return;
  }
  uint32_t delay_ms = (next_refresh_unix - now_sec) * 1000;
  s_refresh_timer = app_timer_register(delay_ms, prv_timer_fired, NULL);
}

static void prv_do_refresh(void) {
  comm_request_arrivals(s_params.query_index, s_params.station_slug, s_params.routes);
}

// ─── Arrivals callback ────────────────────────────────────────────────────────

static void prv_arrivals_received(uint8_t query_index, const ArrivalCache *cache) {
  if (query_index != s_params.query_index) return;
  s_waiting = false;
  if (s_menu) menu_layer_reload_data(s_menu);
  if (cache->next_refresh) prv_schedule_refresh(cache->next_refresh);
}

static void prv_status_received(uint8_t query_index, CommStatus status) {
  if (query_index != s_params.query_index) return;
  s_waiting = false;
  if (s_menu) menu_layer_reload_data(s_menu);
}

// ─── Header layer draw ────────────────────────────────────────────────────────

static void prv_draw_header(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_params.station_name,
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

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t s, void *ctx) {
  if (s_waiting) return 1; // "Loading…"
  const ArrivalCache *c = prv_cache();
  if (!c || !c->valid || c->count == 0) return 1; // "No upcoming trains" or error
  uint8_t rows = c->count;
  if (!s_params.from_favorite) rows++; // "Add to Favorites" row at end
  return rows;
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

  if (s_waiting) {
    graphics_draw_text(ctx, "Loading\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  const ArrivalCache *cache = prv_cache();

  // "Add to Favorites" row (last row in search mode)
  if (!s_params.from_favorite && cache && idx->row == cache->count) {
    menu_cell_basic_draw(ctx, cell, "\xe2\x98\x85 Add to Favorites", NULL, NULL);
    return;
  }

  if (!cache || !cache->valid || cache->count == 0) {
    graphics_draw_text(ctx, "No upcoming trains",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       bounds, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  if (idx->row >= cache->count) return;
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

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  const ArrivalCache *cache = prv_cache();
  if (!s_params.from_favorite && cache && idx->row == cache->count) {
    // "Add to Favorites"
    Favorite fav;
    memset(&fav, 0, sizeof(fav));
    strncpy(fav.station_slug, s_params.station_slug, sizeof(fav.station_slug) - 1);
    strncpy(fav.station_name, s_params.station_name, sizeof(fav.station_name) - 1);

    // Parse routes string "A:E,B:N" back into Favorite.routes
    char routes_copy[64];
    strncpy(routes_copy, s_params.routes, sizeof(routes_copy) - 1);
    routes_copy[sizeof(routes_copy) - 1] = '\0';
    char *tok = strtok(routes_copy, ",");
    while (tok && fav.route_count < MAX_FAV_ROUTES) {
      char *colon = strchr(tok, ':');
      if (colon) {
        strncpy(fav.routes[fav.route_count].route, tok,
                (size_t)(colon - tok) < 4 ? (size_t)(colon - tok) : 3);
        fav.routes[fav.route_count].dir = *(colon + 1);
        fav.route_count++;
      }
      tok = strtok(NULL, ",");
    }
    if (fav.route_count > 0) {
      state_add_favorite(&fav);
      s_params.from_favorite = true;
      s_params.query_index   = state_get_favorite_count() - 1;
      menu_layer_reload_data(s_menu);
      win_home_reload();
    }
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
  s_params  = *params;
  s_waiting = true;

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
