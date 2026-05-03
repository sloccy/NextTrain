#pragma once

#include <pebble.h>
#include "state.h"

// Callbacks registered by windows that care about incoming data.
// Register on window push, clear (pass NULL) on window unload.
typedef void (*ArrivalReceivedCb)(uint8_t query_index, const ArrivalCache *cache);
typedef void (*StationsReadyCb)(void);
typedef void (*StatusReceivedCb)(uint8_t query_index, CommStatus status);

void comm_init(void);
void comm_deinit(void);

void comm_set_arrivals_callback(ArrivalReceivedCb cb);
void comm_set_stations_ready_callback(StationsReadyCb cb);
void comm_set_status_callback(StatusReceivedCb cb);

// Outbound operations — all are queued and serialized through the AppMessage outbox.
// Stations sync takes priority over arrivals requests.
void comm_request_stations_version(void);
void comm_request_stations_full(void);
void comm_request_arrivals(uint8_t query_index, const char *station_slug, const char *routes);
void comm_request_refresh_stations(void);

// Called by main.c's inbox_received handler to route incoming messages.
void comm_inbox_received(DictionaryIterator *iter);
void comm_inbox_dropped(AppMessageResult reason);
void comm_outbox_failed(DictionaryIterator *iter, AppMessageResult reason);
void comm_outbox_sent(DictionaryIterator *iter);
