#include "win_alert_detail.h"
#include "state.h"
#include "comm.h"
#include <string.h>
#include <stdio.h>

// Maximum characters in the assembled alert text buffer.
// 8 alerts × (80 header + 160 desc + separators) ≈ 2.2KB
#define TEXT_BUF_SIZE 2400

static Window      *s_window;
static ScrollLayer *s_scroll;
static TextLayer   *s_text;
static char         s_route[4];
static char         s_text_buf[TEXT_BUF_SIZE];
static bool         s_waiting;

static void prv_build_text(void) {
  const AlertDetailCache *c = state_get_alert_detail();
  if (!c->valid || c->count == 0) {
    strncpy(s_text_buf, "No alerts found.", sizeof(s_text_buf) - 1);
    s_text_buf[sizeof(s_text_buf) - 1] = 0;
    return;
  }
  size_t off = 0;
  for (uint8_t i = 0; i < c->count; i++) {
    const AlertEntry *e = &c->entries[i];
    if (i > 0 && off + 7 < sizeof(s_text_buf)) {
      memcpy(s_text_buf + off, "\n\n---\n\n", 7);
      off += 7;
    }
    size_t hl = strlen(e->header);
    if (hl > 0 && off + hl + 2 < sizeof(s_text_buf)) {
      memcpy(s_text_buf + off, e->header, hl);
      off += hl;
      s_text_buf[off++] = '\n';
      s_text_buf[off++] = '\n';
    }
    size_t dl = strlen(e->desc);
    if (dl > 0 && off + dl + 1 < sizeof(s_text_buf)) {
      memcpy(s_text_buf + off, e->desc, dl);
      off += dl;
    }
    if (off >= sizeof(s_text_buf) - 1) break;
  }
  s_text_buf[off < sizeof(s_text_buf) ? off : sizeof(s_text_buf) - 1] = 0;
}

static void prv_refresh_text(void) {
  prv_build_text();
  if (s_text) {
    text_layer_set_text(s_text, s_text_buf);
    Layer *tl = text_layer_get_layer(s_text);
    GSize size = text_layer_get_content_size(s_text);
    layer_set_frame(tl, GRect(4, 4, layer_get_bounds(scroll_layer_get_layer(s_scroll)).size.w - 8, size.h + 8));
    scroll_layer_set_content_size(s_scroll, GSize(0, size.h + 16));
  }
}

static void prv_detail_received(const AlertDetailCache *cache) {
  s_waiting = false;
  prv_refresh_text();
}

// ─── Window lifecycle ─────────────────────────────────────────────────────────

static void prv_window_load(Window *win) {
  Layer *root  = window_get_root_layer(win);
  GRect bounds = layer_get_bounds(root);

  s_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll, win);
  layer_add_child(root, scroll_layer_get_layer(s_scroll));

  s_text = text_layer_create(GRect(4, 4, bounds.size.w - 8, 2000));
  text_layer_set_font(s_text, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_overflow_mode(s_text, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_text, GColorClear);
  text_layer_set_text_color(s_text, GColorBlack);
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_text));

  comm_set_alert_detail_callback(prv_detail_received);

  // If we already have cached detail data for this route, show immediately.
  const AlertDetailCache *cached = state_get_alert_detail();
  if (cached->valid && strcmp(cached->route, s_route) == 0) {
    s_waiting = false;
    prv_refresh_text();
  } else {
    s_waiting = true;
    strncpy(s_text_buf, "Loading\xe2\x80\xa6", sizeof(s_text_buf) - 1);
    if (s_text) text_layer_set_text(s_text, s_text_buf);
    comm_request_alert_detail(s_route);
  }
}

static void prv_window_unload(Window *win) {
  comm_set_alert_detail_callback(NULL);
  text_layer_destroy(s_text);   s_text = NULL;
  scroll_layer_destroy(s_scroll); s_scroll = NULL;
  window_destroy(win);
  s_window = NULL;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void win_alert_detail_push(const char *route_name) {
  if (s_window) return;
  strncpy(s_route, route_name, sizeof(s_route) - 1);
  s_route[sizeof(s_route) - 1] = 0;

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
