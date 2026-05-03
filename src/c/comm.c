#include "comm.h"
#include "state.h"
#include <string.h>
#include <stdlib.h>

// ─── Outbound queue ───────────────────────────────────────────────────────────

#define QUEUE_SIZE 24

typedef struct {
  bool    used;
  uint8_t op;
  uint8_t index;
  char    station[24];
  char    routes[64];
} QueueEntry;

static QueueEntry s_queue[QUEUE_SIZE];
static bool       s_sending = false;

// ─── Stations assembly ────────────────────────────────────────────────────────

static uint8_t *s_blob       = NULL;
static uint32_t s_blob_used  = 0;
static uint16_t s_total_chunks = 0;
static uint16_t s_recv_chunks  = 0;

// ─── Callbacks ───────────────────────────────────────────────────────────────

static ArrivalReceivedCb s_arr_cb     = NULL;
static StationsReadyCb   s_sta_cb     = NULL;
static StatusReceivedCb  s_status_cb  = NULL;

// ─── Forward declarations ─────────────────────────────────────────────────────

static void prv_send_next(void);
static void prv_handle_stations_version(DictionaryIterator *iter);
static void prv_handle_stations_chunk(DictionaryIterator *iter);
static void prv_handle_arrivals(DictionaryIterator *iter);
static void prv_handle_status(DictionaryIterator *iter);
static ArrivalCache prv_parse_arrivals_payload(const uint8_t *data, size_t len,
                                                const char *station_name,
                                                uint32_t next_refresh);

// ─── Init / Deinit ────────────────────────────────────────────────────────────

void comm_init(void) {
  memset(s_queue, 0, sizeof(s_queue));
  s_sending = false;
}

void comm_deinit(void) {
  if (s_blob) { free(s_blob); s_blob = NULL; }
}

// ─── Callback setters ─────────────────────────────────────────────────────────

void comm_set_arrivals_callback(ArrivalReceivedCb cb)       { s_arr_cb    = cb; }
void comm_set_stations_ready_callback(StationsReadyCb cb)   { s_sta_cb    = cb; }
void comm_set_status_callback(StatusReceivedCb cb)          { s_status_cb = cb; }

// ─── Enqueue helpers ──────────────────────────────────────────────────────────

static bool prv_enqueue(uint8_t op, uint8_t index, const char *station, const char *routes) {
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (!s_queue[i].used) {
      s_queue[i].used  = true;
      s_queue[i].op    = op;
      s_queue[i].index = index;
      if (station) strncpy(s_queue[i].station, station, sizeof(s_queue[i].station) - 1);
      if (routes)  strncpy(s_queue[i].routes,  routes,  sizeof(s_queue[i].routes)  - 1);
      if (!s_sending) prv_send_next();
      return true;
    }
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] queue full");
  return false;
}

// ─── Queue helpers ────────────────────────────────────────────────────────────

static void prv_drop_front_entry(bool notify) {
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (s_queue[i].used) {
      uint8_t idx = s_queue[i].index;
      memset(&s_queue[i], 0, sizeof(QueueEntry));
      if (notify && s_status_cb) s_status_cb(idx, STATUS_ERROR);
      break;
    }
  }
}

// ─── Send a single queued entry ───────────────────────────────────────────────

static void prv_send_next(void) {
  s_sending = false;

  // Find the next used slot (scan from front — FIFO)
  QueueEntry *entry = NULL;
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (s_queue[i].used) { entry = &s_queue[i]; break; }
  }
  if (!entry) return;

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK || !iter) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] outbox begin failed: %d", (int)result);
    prv_drop_front_entry(true);
    return;
  }

  dict_write_uint8(iter, MSG_OP, entry->op);
  dict_write_uint8(iter, MSG_QUERY_INDEX, entry->index);
  if (entry->station[0])
    dict_write_cstring(iter, MSG_QUERY_STATION, entry->station);
  if (entry->routes[0])
    dict_write_cstring(iter, MSG_QUERY_ROUTES, entry->routes);

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] outbox send failed: %d", (int)result);
    return;
  }

  s_sending = true;
}

// ─── Public request functions ─────────────────────────────────────────────────

void comm_request_stations_version(void) {
  prv_enqueue(OP_GET_STATIONS_VERSION, 0, NULL, NULL);
}

void comm_request_stations_full(void) {
  prv_enqueue(OP_GET_STATIONS_FULL, 0, NULL, NULL);
}

void comm_request_arrivals(uint8_t query_index, const char *station_slug, const char *routes) {
  prv_enqueue(OP_GET_ARRIVALS, query_index, station_slug, routes);
}

void comm_request_refresh_stations(void) {
  prv_enqueue(OP_REFRESH_STATIONS, 0, NULL, NULL);
}

// ─── Outbox callbacks ─────────────────────────────────────────────────────────

void comm_outbox_sent(DictionaryIterator *iter) {
  prv_drop_front_entry(false);
  prv_send_next();
}

void comm_outbox_failed(DictionaryIterator *iter, AppMessageResult reason) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] outbox failed: %d, dropping entry", (int)reason);
  s_sending = false;
  prv_drop_front_entry(true);
  prv_send_next();
}

void comm_inbox_dropped(AppMessageResult reason) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] inbox dropped: %d", (int)reason);
}

// ─── Inbox dispatcher ─────────────────────────────────────────────────────────

void comm_inbox_received(DictionaryIterator *iter) {
  Tuple *dt = dict_find(iter, MSG_DATA_TYPE);
  if (dt) {
    switch ((DataType)dt->value->uint8) {
      case DATA_TYPE_STATIONS_VERSION: prv_handle_stations_version(iter); return;
      case DATA_TYPE_STATIONS_CHUNK:   prv_handle_stations_chunk(iter);   return;
      case DATA_TYPE_ARRIVALS:         prv_handle_arrivals(iter);          return;
    }
  }
  Tuple *st = dict_find(iter, MSG_STATUS);
  if (st) prv_handle_status(iter);
}

// ─── Inbound handlers ─────────────────────────────────────────────────────────

static void prv_handle_stations_version(DictionaryIterator *iter) {
  Tuple *vt = dict_find(iter, MSG_STATIONS_VERSION);
  if (!vt) return;
  uint32_t phone_version = vt->value->uint32;
  uint32_t watch_version = state_get_persisted_stations_version();

  APP_LOG(APP_LOG_LEVEL_DEBUG, "[comm] stations version: phone=%lu watch=%lu",
          (unsigned long)phone_version, (unsigned long)watch_version);

  if (phone_version != watch_version) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] stations stale, requesting full sync");
    comm_request_stations_full();
  } else {
    if (state_load_stations_from_persist()) {
      if (s_sta_cb) s_sta_cb();
    } else {
      comm_request_stations_full();
    }
  }
}

static void prv_handle_stations_chunk(DictionaryIterator *iter) {
  Tuple *ci = dict_find(iter, MSG_CHUNK_INDEX);
  Tuple *ct = dict_find(iter, MSG_CHUNK_TOTAL);
  Tuple *pl = dict_find(iter, MSG_PAYLOAD);
  if (!ci || !ct || !pl) return;

  uint16_t chunk_index = ci->value->uint16;
  uint16_t chunk_total = ct->value->uint16;
  const uint8_t *data  = pl->value->data;
  uint16_t data_len    = pl->length;

  if (chunk_index == 0) {
    // First chunk: allocate assembly buffer
    if (s_blob) free(s_blob);
    s_blob = malloc(16384); // max expected blob size
    s_blob_used    = 0;
    s_total_chunks = chunk_total;
    s_recv_chunks  = 0;
  }

  if (!s_blob || chunk_index != s_recv_chunks) return; // out-of-order

  if (s_blob_used + data_len <= 16384) {
    memcpy(s_blob + s_blob_used, data, data_len);
    s_blob_used += data_len;
  }
  s_recv_chunks++;

  APP_LOG(APP_LOG_LEVEL_DEBUG, "[comm] stations chunk %u/%u", chunk_index + 1, chunk_total);

  if (s_recv_chunks == s_total_chunks) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] stations sync complete (%lu bytes)", (unsigned long)s_blob_used);
    state_persist_stations_blob(s_blob, s_blob_used);
    free(s_blob); s_blob = NULL;
    if (state_load_stations_from_persist()) {
      if (s_sta_cb) s_sta_cb();
    }
  }
}

static void prv_handle_arrivals(DictionaryIterator *iter) {
  Tuple *qi = dict_find(iter, MSG_QUERY_INDEX);
  Tuple *sn = dict_find(iter, MSG_STATION_NAME);
  Tuple *nr = dict_find(iter, MSG_NEXT_REFRESH);
  Tuple *pl = dict_find(iter, MSG_PAYLOAD);
  if (!qi || !pl) return;

  uint8_t query_index  = qi->value->uint8;
  const char *sta_name = sn ? sn->value->cstring : "Unknown";
  uint32_t next_ref    = nr ? nr->value->uint32 : 0;

  ArrivalCache cache = prv_parse_arrivals_payload(pl->value->data, pl->length,
                                                   sta_name, next_ref);
  state_set_arrival_cache(query_index, &cache);
  if (s_arr_cb) s_arr_cb(query_index, &cache);
}

static void prv_handle_status(DictionaryIterator *iter) {
  Tuple *st = dict_find(iter, MSG_STATUS);
  Tuple *qi = dict_find(iter, MSG_QUERY_INDEX);
  if (!st) return;
  uint8_t status      = st->value->uint8;
  uint8_t query_index = qi ? qi->value->uint8 : QUERY_INDEX_TRANSIENT;
  if (s_status_cb) s_status_cb(query_index, (StatusCode)status);
}

// ─── Arrivals payload parser ──────────────────────────────────────────────────

static ArrivalCache prv_parse_arrivals_payload(const uint8_t *data, size_t len,
                                                const char *station_name,
                                                uint32_t next_refresh) {
  ArrivalCache cache = {0};
  strncpy(cache.station_name, station_name, sizeof(cache.station_name) - 1);
  cache.next_refresh = next_refresh;

  if (!data || len < 1) return cache;
  const uint8_t *p   = data;
  const uint8_t *end = data + len;

  uint8_t count = *p++;
  if (count > MAX_ARRIVALS) count = MAX_ARRIVALS;

  for (uint8_t i = 0; i < count; i++) {
    ArrivalEntry *e = &cache.entries[i];

    if (p + 4 > end) goto done;
    e->r      = *p++;
    e->g      = *p++;
    e->b      = *p++;
    e->status = (ArrivalStatus)*p++;

    // lpStr: [u8 len][bytes] — goto done exits the for loop on truncated data
    #define LP(field) do { \
      if (p >= end) goto done; \
      uint8_t _l = *p++; \
      if (p + _l > end) goto done; \
      uint8_t _c = _l < sizeof(e->field) - 1 ? _l : sizeof(e->field) - 1; \
      memcpy(e->field, p, _c); e->field[_c] = 0; p += _l; \
    } while(0)

    LP(route);
    LP(headsign);
    LP(time);
    LP(label);

    #undef LP
    cache.count++;
  }
done:

  cache.valid = true;
  return cache;
}
