#include "win_home.h"
#include "win_picker.h"
#include "win_arrivals.h"
#include "win_alerts.h"
#include "win_settings.h"
#include "state.h"
#include "comm.h"
#include "ui.h"
#include "format.h"
#include <string.h>

// ─── Row geometry ─────────────────────────────────────────────────────────────

#define ROW_HEIGHT_FAV    NT_ROW_H_FAV     // 56
#define ROW_HEIGHT_ACTION NT_ROW_H_ACTION  // 40
#define ICON_SIZE         24
#define ICON_MARGIN_LEFT   8
#define TEXT_MARGIN_LEFT   8

typedef enum { KIND_FAVORITES, KIND_RECENT, KIND_ACTIONS } SectionKind;

// ─── State ────────────────────────────────────────────────────────────────────

static Window      *s_window;
static MenuLayer   *s_menu;
static ActionMenu  *s_action_menu;

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

static bool prv_recent_visible(void) {
  RecentSearch rec;
  return state_get_recent_search(&rec) &&
         state_get_show_recent() &&
         !state_is_recent_dismissed();
}

static SectionKind prv_section_kind(uint16_t section) {
  if (section == 0) return KIND_FAVORITES;
  if (section == 1 && prv_recent_visible()) return KIND_RECENT;
  return KIND_ACTIONS;
}

static void prv_build_recent_fav(const RecentSearch *rec, Favorite *out) {
  memset(out, 0, sizeof(Favorite));
  strncpy(out->station_slug, rec->station_slug, sizeof(out->station_slug) - 1);
  const char *p = rec->routes;
  while (*p && *(p + 1) && out->route_count < MAX_FAV_ROUTES) {
    out->routes[out->route_count].route[0] = *p;
    out->routes[out->route_count].route[1] = '\0';
    switch (*(p + 1)) {
      case '1': out->routes[out->route_count].dir = 'S'; break;
      case '2': out->routes[out->route_count].dir = 'E'; break;
      case '3': out->routes[out->route_count].dir = 'W'; break;
      default:  out->routes[out->route_count].dir = 'N'; break;
    }
    out->route_count++;
    p += 2;
  }
}

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
  return (uint16_t)(2 + (prv_recent_visible() ? 1 : 0));
}

static uint16_t prv_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  switch (prv_section_kind(section)) {
    case KIND_FAVORITES: {
      uint8_t n = state_get_favorite_count();
      return n > 0 ? n : 1; // 1 = empty-state placeholder
    }
    case KIND_RECENT:  return 1;
    case KIND_ACTIONS: return 3; // "Alerts", "New Search", "Settings"
  }
  return 0;
}

static int16_t prv_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  SectionKind k = prv_section_kind(idx->section);
  return (k == KIND_ACTIONS) ? ROW_HEIGHT_ACTION : ROW_HEIGHT_FAV;
}

static int16_t prv_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return prv_section_kind(section) == KIND_RECENT ? 16 : 0;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *c) {
  if (prv_section_kind(section) != KIND_RECENT) return;
  GRect bounds = layer_get_bounds(cell);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "RECENT",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(8, 0, 60, bounds.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  int16_t y = bounds.size.h / 2 + 2;
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(56, y), GPoint(bounds.size.w - 8, y));
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect bounds = layer_get_bounds(cell);
  bool  highlight = menu_cell_layer_is_highlighted(cell);

  // White background always (even when highlighted we draw our own fill)
  graphics_context_set_fill_color(ctx, highlight ? GColorLightGray : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  SectionKind kind = prv_section_kind(idx->section);

  if (kind == KIND_RECENT) {
    RecentSearch rec;
    if (!state_get_recent_search(&rec)) return;

    Favorite tmp_fav;
    prv_build_recent_fav(&rec, &tmp_fav);
    StationsCache *stations = state_get_stations();

    GPoint icon_origin = GPoint(ICON_MARGIN_LEFT,
                                bounds.origin.y + (bounds.size.h - 44) / 2);
    int16_t x = ui_draw_favorite_icon(ctx, icon_origin, &tmp_fav, stations);
    graphics_context_set_text_color(ctx, GColorBlack);

    x += TEXT_MARGIN_LEFT;
    char display_name[40];
    slug_to_display(rec.station_slug, display_name, sizeof(display_name));

    const int16_t NAME_BOX_H  = 22;
    const int16_t SUB_BOX_H   = 16;
    const int16_t VISUAL_NUDGE = 4;
    int16_t icon_cy  = bounds.size.h / 2;
    int16_t text_w   = bounds.size.w - x - 4;
    int16_t stack_h  = NAME_BOX_H + SUB_BOX_H;
    int16_t name_top = icon_cy - stack_h / 2 - VISUAL_NUDGE;

    graphics_draw_text(ctx, display_name,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(x, name_top, text_w, NAME_BOX_H),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, "Tap to view",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(x, name_top + NAME_BOX_H, text_w, SUB_BOX_H),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  if (kind == KIND_FAVORITES) {
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
      uint16_t pred = cache->entries[0].mins;
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
  const char *labels[] = {"Alerts", "+ New Search", "Settings"};
  if (idx->row < 3) {
    menu_cell_basic_draw(ctx, cell, labels[idx->row], NULL, NULL);
  }
}

static void prv_dismiss_action_performed(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  (void)am; (void)item; (void)ctx;
  state_set_recent_dismissed(true);
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  switch (prv_section_kind(idx->section)) {
    case KIND_FAVORITES:
      if (state_get_favorite_count() == 0) { win_station_picker_push(); return; }
      prv_launch_favorite(idx->row);
      break;
    case KIND_RECENT: {
      RecentSearch rec;
      if (!state_get_recent_search(&rec)) break;
      ArrivalsParams params;
      memset(&params, 0, sizeof(params));
      strncpy(params.station_slug, rec.station_slug, sizeof(params.station_slug) - 1);
      strncpy(params.routes,       rec.routes,       sizeof(params.routes) - 1);
      params.query_index   = QUERY_INDEX_TRANSIENT;
      params.from_favorite = false;
      win_arrivals_push(&params);
      break;
    }
    case KIND_ACTIONS:
      if (idx->row == 0)      win_alerts_push();
      else if (idx->row == 1) win_station_picker_push();
      else                    win_settings_push();
      break;
  }
}

static void prv_select_long(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (prv_section_kind(idx->section) != KIND_RECENT) return;
  ActionMenuLevel *root = action_menu_level_create(1);
  action_menu_level_add_action(root, "Dismiss", prv_dismiss_action_performed, NULL);
  ActionMenuConfig cfg = {
    .root_level = root,
    .colors     = { .background = GColorWhite, .foreground = GColorBlack },
    .align      = ActionMenuAlignTop,
  };
  s_action_menu = action_menu_open(&cfg);
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
    .draw_header       = prv_draw_header,
    .draw_row          = prv_draw_row,
    .select_click      = prv_select,
    .select_long_click = prv_select_long,
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
