#include <pebble.h>
#include <string.h>
#include "protocol.h"
#include "state.h"
#include "comm.h"
#include "win_home.h"
#include "win_arrivals.h"


// ─── Background prefetch ──────────────────────────────────────────────────────

// Fire OP_GET_ARRIVALS for each saved favorite after stations version check.
static void prv_prefetch_favorites(void) {
  uint8_t count = state_get_favorite_count();
  for (uint8_t i = 0; i < count; i++) {
    Favorite *fav = state_get_favorite(i);
    if (!fav) continue;

    char routes[64] = {0};
    state_format_routes_query(fav, routes, sizeof(routes));
    comm_request_arrivals(i, fav->station_slug, routes);
  }
}

// ─── AppMessage callbacks ─────────────────────────────────────────────────────

static void prv_inbox_received(DictionaryIterator *iter, void *ctx) {
  comm_inbox_received(iter);
}

static void prv_inbox_dropped(AppMessageResult reason, void *ctx) {
  comm_inbox_dropped(reason);
}

static void prv_outbox_sent(DictionaryIterator *iter, void *ctx) {
  comm_outbox_sent(iter);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  comm_outbox_failed(iter, reason);
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

static void prv_init(void) {
  state_init();
  comm_init();

  // Open AppMessage
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  // Push home first — renders immediately from persist
  win_home_push();

  // Background: check stations version + prefetch favorite arrivals
  comm_request_stations_version();
  prv_prefetch_favorites();
}

static void prv_deinit(void) {
  comm_deinit();
  state_deinit();
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
  return 0;
}
