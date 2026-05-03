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
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] init");
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
  if (!entry) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "[comm] send_next: queue empty");
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] send_next: op=%d idx=%d sta='%s'",
          (int)entry->op, (int)entry->index,
          entry->station[0] ? entry->station : "(none)");

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK || !iter) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] outbox_begin failed: %d", (int)result);
    prv_drop_front_entry(true);
    return;
  }

  dict_write_uint8(iter, MESSAGE_KEY_OP, entry->op);
  dict_write_uint8(iter, MESSAGE_KEY_QUERY_INDEX, entry->index);
  if (entry->station[0])
    dict_write_cstring(iter, MESSAGE_KEY_QUERY_STATION, entry->station);
  if (entry->routes[0])
    dict_write_cstring(iter, MESSAGE_KEY_QUERY_ROUTES, entry->routes);

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] outbox_send failed: %d", (int)result);
    return;
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "[comm] outbox_send OK");
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
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] outbox_sent ACK");
  prv_drop_front_entry(false);
  prv_send_next();
}

void comm_outbox_failed(DictionaryIterator *iter, AppMessageResult reason) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] outbox_failed reason=%d, dropping entry", (int)reason);
  s_sending = false;
  prv_drop_front_entry(true);
  prv_send_next();
}

void comm_inbox_dropped(AppMessageResult reason) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] inbox_dropped reason=%d (message lost!)", (int)reason);
}

// ─── Inbox dispatcher ─────────────────────────────────────────────────────────

void comm_inbox_received(DictionaryIterator *iter) {
  Tuple *dt = dict_find(iter, MESSAGE_KEY_DATA_TYPE);
  Tuple *st = dict_find(iter, MESSAGE_KEY_STATUS);
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] inbox_received: data_type=%s status=%s",
          dt ? "YES" : "NO", st ? "YES" : "NO");
  if (dt) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm]   data_type=%d", (int)dt->value->uint8);
    switch ((DataType)dt->value->uint8) {
      case DATA_TYPE_STATIONS_VERSION: prv_handle_stations_version(iter); return;
      case DATA_TYPE_STATIONS_CHUNK:   prv_handle_stations_chunk(iter);   return;
      case DATA_TYPE_ARRIVALS:         prv_handle_arrivals(iter);          return;
      default:
        APP_LOG(APP_LOG_LEVEL_WARNING, "[comm]   unknown data_type=%d", (int)dt->value->uint8);
        return;
    }
  }
  if (st) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm]   status msg, value=%d", (int)st->value->uint8);
    prv_handle_status(iter);
    return;
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] inbox_received: no data_type or status key!");
}

// ─── Inbound handlers ─────────────────────────────────────────────────────────

static void prv_handle_stations_version(DictionaryIterator *iter) {
  Tuple *vt = dict_find(iter, MESSAGE_KEY_STATIONS_VERSION);
  if (!vt) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] stations_version msg missing version key!");
    return;
  }
  uint32_t phone_version = vt->value->uint32;
  uint32_t watch_version = state_get_persisted_stations_version();

  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] stations_version: phone=%lu watch=%lu",
          (unsigned long)phone_version, (unsigned long)watch_version);

  if (phone_version != watch_version) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] versions differ, requesting full sync");
    comm_request_stations_full();
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] versions match, loading from persist");
    if (state_load_stations_from_persist()) {
      APP_LOG(APP_LOG_LEVEL_INFO, "[comm] persist load OK, firing sta_cb=%s",
              s_sta_cb ? "SET" : "NULL");
      if (s_sta_cb) s_sta_cb();
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] persist load FAILED, requesting full sync");
      comm_request_stations_full();
    }
  }
}

static void prv_handle_stations_chunk(DictionaryIterator *iter) {
  Tuple *ci = dict_find(iter, MESSAGE_KEY_CHUNK_INDEX);
  Tuple *ct = dict_find(iter, MESSAGE_KEY_CHUNK_TOTAL);
  Tuple *pl = dict_find(iter, MESSAGE_KEY_PAYLOAD);
  if (!ci || !ct || !pl) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] chunk missing keys: ci=%s ct=%s pl=%s",
            ci ? "OK" : "MISSING", ct ? "OK" : "MISSING", pl ? "OK" : "MISSING");
    return;
  }

  uint16_t chunk_index = ci->value->uint16;
  uint16_t chunk_total = ct->value->uint16;
  const uint8_t *data  = pl->value->data;
  uint16_t data_len    = pl->length;

  if (chunk_index == 0) {
    if (s_blob) free(s_blob);
    s_blob = malloc(16384);
    s_blob_used    = 0;
    s_total_chunks = chunk_total;
    s_recv_chunks  = 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] chunk alloc: total_chunks=%u blob=%s",
            chunk_total, s_blob ? "OK" : "MALLOC FAILED");
  }

  if (!s_blob) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] chunk: no blob buffer");
    return;
  }
  if (chunk_index != s_recv_chunks) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] chunk out-of-order: got=%u expected=%u",
            chunk_index, s_recv_chunks);
    return;
  }

  if (s_blob_used + data_len <= 16384) {
    memcpy(s_blob + s_blob_used, data, data_len);
    s_blob_used += data_len;
  }
  s_recv_chunks++;

  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] chunk %u/%u (%u bytes, total_so_far=%lu)",
          chunk_index + 1, chunk_total, data_len, (unsigned long)s_blob_used);

  if (s_recv_chunks == s_total_chunks) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] all chunks received, blob=%lu bytes",
            (unsigned long)s_blob_used);
    state_persist_stations_blob(s_blob, s_blob_used);
    free(s_blob); s_blob = NULL;
    if (state_load_stations_from_persist()) {
      APP_LOG(APP_LOG_LEVEL_INFO, "[comm] stations loaded from persist, sta_cb=%s",
              s_sta_cb ? "SET" : "NULL");
      if (s_sta_cb) s_sta_cb();
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] persist load FAILED after full sync!");
    }
  }
}

static void prv_handle_arrivals(DictionaryIterator *iter) {
  Tuple *qi = dict_find(iter, MESSAGE_KEY_QUERY_INDEX);
  Tuple *sn = dict_find(iter, MESSAGE_KEY_STATION_NAME);
  Tuple *nr = dict_find(iter, MESSAGE_KEY_NEXT_REFRESH);
  Tuple *pl = dict_find(iter, MESSAGE_KEY_PAYLOAD);
  if (!qi || !pl) return;

  uint8_t query_index  = qi->value->uint8;
  const char *sta_name = sn ? sn->value->cstring : "Unknown";
  uint32_t next_ref    = nr ? nr->value->uint32 : 0;

  ArrivalCache cache = prv_parse_arrivals_payload(pl->value->data, pl->length,
                                                   sta_name, next_ref);
  state_set_arrival_cache(query_index, &cache);
  if (s_arr_cb) s_arr_cb(query_index, &cache);
}

static const char *prv_status_name(CommStatus s) {
  switch (s) {
    case STATUS_OK:      return "OK";
    case STATUS_OFFLINE: return "OFFLINE";
    case STATUS_NO_DATA: return "NO_DATA";
    case STATUS_ERROR:   return "ERROR";
    default:             return "UNKNOWN";
  }
}

static void prv_handle_status(DictionaryIterator *iter) {
  Tuple *st = dict_find(iter, MESSAGE_KEY_STATUS);
  Tuple *qi = dict_find(iter, MESSAGE_KEY_QUERY_INDEX);
  if (!st) return;
  uint8_t status      = st->value->uint8;
  uint8_t query_index = qi ? qi->value->uint8 : QUERY_INDEX_TRANSIENT;
  APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] status: %s (%d) for query_index=%d, cb=%s",
          prv_status_name((CommStatus)status), (int)status, (int)query_index,
          s_status_cb ? "SET" : "NULL");
  if (s_status_cb) s_status_cb(query_index, (CommStatus)status);
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
