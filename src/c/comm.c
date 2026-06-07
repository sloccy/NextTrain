#include "comm.h"
#include "state.h"
#include <string.h>
#include <stdlib.h>

// ─── Inbox size (set by main.c after app_message_open) ───────────────────────

static uint32_t s_inbox_size = 0;

// ─── Outbound queue ───────────────────────────────────────────────────────────

#define QUEUE_SIZE 24

typedef struct {
  bool    used;
  uint8_t op;
  uint8_t index;
  char    station[40];
  char    routes[64];
} QueueEntry;

static QueueEntry s_queue[QUEUE_SIZE];
static bool       s_sending = false;

// ─── Stations assembly ────────────────────────────────────────────────────────

static uint8_t *s_blob       = NULL;
static uint32_t s_blob_used  = 0;
static uint16_t s_total_chunks = 0;
static uint16_t s_recv_chunks  = 0;

// ─── Alert detail assembly ────────────────────────────────────────────────────

static uint8_t  *s_al_blob         = NULL;
static uint32_t  s_al_blob_used    = 0;
static uint16_t  s_al_total_chunks = 0;
static uint16_t  s_al_recv_chunks  = 0;

// ─── Callbacks ───────────────────────────────────────────────────────────────

static ArrivalReceivedCb       s_arr_cb       = NULL;
static StationsReadyCb         s_sta_cb       = NULL;
static StatusReceivedCb        s_status_cb    = NULL;
static FavoriteRenamedCb       s_fav_cb       = NULL;
static AlertSummaryReceivedCb  s_al_sum_cb    = NULL;
static AlertDetailReceivedCb   s_al_det_cb    = NULL;
static char                    s_pending_alert_route[4] = {0};

// ─── Forward declarations ─────────────────────────────────────────────────────

static void prv_send_next(void);
static void prv_handle_stations_version(DictionaryIterator *iter);
static void prv_handle_stations_chunk(DictionaryIterator *iter);
static void prv_handle_arrivals(DictionaryIterator *iter);
static void prv_handle_status(DictionaryIterator *iter);
static void prv_handle_favorites_request(void);
static void prv_handle_rename_favorite(DictionaryIterator *iter);
static void prv_handle_alerts_summary(DictionaryIterator *iter);
static void prv_parse_alert_detail(const uint8_t *data, size_t len);
static void prv_handle_alert_detail_chunk(DictionaryIterator *iter);
static void prv_parse_arrivals_payload(ArrivalCache *cache,
                                        const uint8_t *data, size_t len,
                                        uint32_t next_refresh);

// ─── Init / Deinit ────────────────────────────────────────────────────────────

void comm_init(uint32_t inbox_size) {
  s_inbox_size = inbox_size;
  memset(s_queue, 0, sizeof(s_queue));
  s_sending = false;
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] init inbox_size=%lu", (unsigned long)inbox_size);
}

void comm_deinit(void) {
  if (s_blob)    { free(s_blob);    s_blob    = NULL; }
  if (s_al_blob) { free(s_al_blob); s_al_blob = NULL; }
}

// ─── Callback setters ─────────────────────────────────────────────────────────

void comm_set_arrivals_callback(ArrivalReceivedCb cb)       { s_arr_cb    = cb; }
void comm_set_stations_ready_callback(StationsReadyCb cb)   { s_sta_cb    = cb; }
void comm_set_status_callback(StatusReceivedCb cb)          { s_status_cb = cb; }
void comm_set_favorite_renamed_callback(FavoriteRenamedCb cb) { s_fav_cb  = cb; }
void comm_set_alert_summary_callback(AlertSummaryReceivedCb cb) { s_al_sum_cb = cb; }
void comm_set_alert_detail_callback(AlertDetailReceivedCb cb)   { s_al_det_cb = cb; }

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
  if (entry->op == OP_GET_STATIONS_FULL)
    dict_write_uint32(iter, MESSAGE_KEY_INBOX_SIZE, s_inbox_size);
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

void comm_request_alerts_summary(void) {
  prv_enqueue(OP_GET_ALERTS_SUMMARY, 0, NULL, NULL);
}

void comm_request_alert_detail(const char *route_name) {
  strncpy(s_pending_alert_route, route_name, sizeof(s_pending_alert_route) - 1);
  s_pending_alert_route[sizeof(s_pending_alert_route) - 1] = 0;
  prv_enqueue(OP_GET_ALERT_DETAIL, 0, NULL, route_name);
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
      case DATA_TYPE_STATIONS_VERSION:  prv_handle_stations_version(iter); return;
      case DATA_TYPE_STATIONS_CHUNK:    prv_handle_stations_chunk(iter);   return;
      case DATA_TYPE_ARRIVALS:          prv_handle_arrivals(iter);         return;
      case DATA_TYPE_ALERTS_SUMMARY:    prv_handle_alerts_summary(iter);   return;
      case DATA_TYPE_ALERT_DETAIL_CHUNK: prv_handle_alert_detail_chunk(iter); return;
      case DATA_TYPE_FAVORITES_REQUEST: prv_handle_favorites_request();    return;
      case DATA_TYPE_RENAME_FAVORITE:   prv_handle_rename_favorite(iter);  return;
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

  if (s_blob_used + data_len > 16384) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] chunk overflow: used=%lu len=%u",
            (unsigned long)s_blob_used, data_len);
    free(s_blob); s_blob = NULL;
    return;
  }
  memcpy(s_blob + s_blob_used, data, data_len);
  s_blob_used += data_len;
  s_recv_chunks++;

  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] chunk %u/%u (%u bytes, total_so_far=%lu)",
          chunk_index + 1, chunk_total, data_len, (unsigned long)s_blob_used);

  if (s_recv_chunks == s_total_chunks) {
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] all chunks received, blob=%lu bytes",
            (unsigned long)s_blob_used);
    bool ok = state_load_stations_from_buffer(s_blob, s_blob_used);
    free(s_blob); s_blob = NULL;
    if (ok) {
      APP_LOG(APP_LOG_LEVEL_INFO, "[comm] stations parsed from RAM, sta_cb=%s",
              s_sta_cb ? "SET" : "NULL");
      // Persist a favorites-only subset for fast cold-start render. The full
      // blob can't fit in Pebble's 4KB per-app persist quota.
      state_persist_favorite_stations();
      if (s_sta_cb) s_sta_cb();
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] stations parse FAILED after full sync!");
    }
  }
}

static void prv_handle_arrivals(DictionaryIterator *iter) {
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] handle_arrivals: enter");
  Tuple *qi = dict_find(iter, MESSAGE_KEY_QUERY_INDEX);
  Tuple *nr = dict_find(iter, MESSAGE_KEY_NEXT_REFRESH);
  Tuple *pl = dict_find(iter, MESSAGE_KEY_PAYLOAD);
  if (!qi || !pl) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] handle_arrivals: missing qi=%s pl=%s",
            qi ? "OK" : "MISSING", pl ? "OK" : "MISSING");
    return;
  }

  uint8_t query_index = qi->value->uint8;
  uint32_t next_ref   = nr ? nr->value->uint32 : 0;
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] handle_arrivals: qi=%d pl_len=%u next=%lu",
          (int)query_index, (unsigned)pl->length, (unsigned long)next_ref);

  // Parse directly into the global cache slot — keeps ArrivalCache off the stack
  // (returning by value blew the app stack and crashed the watch).
  ArrivalCache *cache = state_get_arrival_cache(query_index);
  state_clear_arrival_cache(query_index);
  prv_parse_arrivals_payload(cache, pl->value->data, pl->length, next_ref);
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] handle_arrivals: parsed count=%d cb=%s",
          (int)cache->count, s_arr_cb ? "SET" : "NULL");
  if (s_arr_cb) s_arr_cb(query_index, cache);
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] handle_arrivals: done");
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

// ─── Phone config handlers ────────────────────────────────────────────────────

static void prv_handle_favorites_request(void) {
  if (s_sending) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] favorites_request: outbox busy, skipping");
    return;
  }

  uint8_t count = state_get_favorite_count();
  char buf[1024];
  int off = 0;

  buf[off++] = '[';
  for (uint8_t i = 0; i < count; i++) {
    Favorite *f = state_get_favorite(i);
    if (!f) continue;
    char entry[128]; // max: comma + {"i":127,"s":"<39>","n":"<23>"} ≈ 87 bytes
    int n = snprintf(entry, sizeof(entry), "%s{\"i\":%u,\"s\":\"%s\",\"n\":\"%s\"}",
                     off > 1 ? "," : "",
                     (unsigned)i, f->station_slug, f->name);
    if (n <= 0 || n >= (int)sizeof(entry)) break; // entry too long (shouldn't happen)
    if (off + n + 2 > (int)sizeof(buf)) break;     // won't fit with trailing ']' + '\0'
    memcpy(buf + off, entry, (size_t)n);
    off += n;
  }
  if (off < (int)sizeof(buf) - 1) buf[off++] = ']';
  buf[off] = 0;

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK || !iter) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] favorites_request: outbox_begin failed: %d", (int)result);
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_DATA_TYPE, DATA_TYPE_FAVORITES_LIST);
  dict_write_cstring(iter, MESSAGE_KEY_PAYLOAD, buf);
  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] favorites_list sent (%d bytes)", off);
}

static void prv_handle_rename_favorite(DictionaryIterator *iter) {
  Tuple *idx_t  = dict_find(iter, MESSAGE_KEY_RENAME_INDEX);
  Tuple *name_t = dict_find(iter, MESSAGE_KEY_RENAME_NAME);
  if (!idx_t || !name_t) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "[comm] rename_favorite: missing keys");
    return;
  }
  uint8_t idx = idx_t->value->uint8;
  state_set_favorite_name(idx, name_t->value->cstring);
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] favorite %u renamed to '%s'", (unsigned)idx, name_t->value->cstring);
  if (s_fav_cb) s_fav_cb();
}

// ─── Alerts handlers ─────────────────────────────────────────────────────────

static void prv_handle_alerts_summary(DictionaryIterator *iter) {
  Tuple *pl = dict_find(iter, MESSAGE_KEY_PAYLOAD);
  if (!pl) return;
  const uint8_t *p   = pl->value->data;
  const uint8_t *end = p + pl->length;
  uint8_t route_count, i, nlen, copy;

  AlertSummaryCache cache;
  memset(&cache, 0, sizeof(cache));
  cache.valid = true;

  if (p >= end) goto sum_done;
  route_count = *p++;
  for (i = 0; i < route_count && cache.count < MAX_ALERT_ROUTES; i++) {
    if (p >= end) break;
    nlen = *p++;
    if (p + nlen > end) break;
    copy = nlen < sizeof(cache.routes[cache.count].name) - 1
         ? nlen : (uint8_t)(sizeof(cache.routes[cache.count].name) - 1);
    memcpy(cache.routes[cache.count].name, p, copy);
    cache.routes[cache.count].name[copy] = 0;
    p += nlen;
    if (p >= end) break;
    cache.routes[cache.count].count = *p++;
    cache.count++;
  }

sum_done:
  state_set_alert_summary(&cache);
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] alerts_summary: %d routes", (int)cache.count);
  if (s_al_sum_cb) s_al_sum_cb(&cache);
}

static void prv_parse_alert_detail(const uint8_t *data, size_t len) {
  const uint8_t *p   = data;
  const uint8_t *end = data + len;
  uint8_t  alert_count, i, hl, hcopy;
  uint16_t dl, dcopy;
  AlertEntry *e;

  // Parse directly into global — AlertDetailCache is ~8KB; putting it on the
  // stack blows the Pebble app stack and crashes with PC:0 LR:0.
  AlertDetailCache *cache = state_get_alert_detail();
  memset(cache, 0, sizeof(*cache));
  cache->valid = true;

  if (p >= end) goto det_done;
  alert_count = *p++;
  for (i = 0; i < alert_count && cache->count < MAX_ALERTS_PER_ROUTE; i++) {
    e = &cache->entries[cache->count];
    if (p >= end) break;
    hl = *p++;
    if (p + hl > end) break;
    hcopy = hl < sizeof(e->header) - 1 ? hl : (uint8_t)(sizeof(e->header) - 1);
    memcpy(e->header, p, hcopy); e->header[hcopy] = 0;
    p += hl;
    if (p + 1 >= end) break;
    dl  = (uint16_t)*p++;
    dl |= (uint16_t)*p++ << 8;
    if (p + dl > end) break;
    dcopy = dl < sizeof(e->desc) - 1 ? (uint16_t)dl : (uint16_t)(sizeof(e->desc) - 1);
    memcpy(e->desc, p, dcopy); e->desc[dcopy] = 0;
    p += dl;
    cache->count++;
  }

det_done:
  strncpy(cache->route, s_pending_alert_route, sizeof(cache->route) - 1);
  cache->route[sizeof(cache->route) - 1] = 0;
  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] alert_detail: %d alerts", (int)cache->count);
  if (s_al_det_cb) s_al_det_cb(cache);
}

static void prv_handle_alert_detail_chunk(DictionaryIterator *iter) {
  Tuple *ci = dict_find(iter, MESSAGE_KEY_CHUNK_INDEX);
  Tuple *ct = dict_find(iter, MESSAGE_KEY_CHUNK_TOTAL);
  Tuple *pl = dict_find(iter, MESSAGE_KEY_PAYLOAD);
  if (!ci || !ct || !pl) return;

  uint16_t chunk_index = ci->value->uint16;
  uint16_t chunk_total = ct->value->uint16;
  const uint8_t *data  = pl->value->data;
  uint16_t data_len    = pl->length;

  if (chunk_index == 0) {
    if (s_al_blob) free(s_al_blob);
    s_al_blob = malloc(9216);
    s_al_blob_used    = 0;
    s_al_total_chunks = chunk_total;
    s_al_recv_chunks  = 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "[comm] al_chunk alloc: total=%u buf=%s",
            chunk_total, s_al_blob ? "OK" : "FAILED");
  }

  if (!s_al_blob) return;
  if (chunk_index != s_al_recv_chunks) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] al_chunk out-of-order: got=%u expected=%u",
            chunk_index, s_al_recv_chunks);
    return;
  }

  if (s_al_blob_used + data_len > 9216) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[comm] al_chunk overflow: used=%lu len=%u",
            (unsigned long)s_al_blob_used, data_len);
    free(s_al_blob); s_al_blob = NULL;
    return;
  }
  memcpy(s_al_blob + s_al_blob_used, data, data_len);
  s_al_blob_used += data_len;
  s_al_recv_chunks++;

  APP_LOG(APP_LOG_LEVEL_INFO, "[comm] al_chunk %u/%u (%u B, total=%lu)",
          chunk_index + 1, chunk_total, data_len, (unsigned long)s_al_blob_used);

  if (s_al_recv_chunks == s_al_total_chunks) {
    prv_parse_alert_detail(s_al_blob, s_al_blob_used);
    free(s_al_blob); s_al_blob = NULL;
  }
}

// ─── Arrivals payload parser ──────────────────────────────────────────────────

static void prv_parse_arrivals_payload(ArrivalCache *cache,
                                        const uint8_t *data, size_t len,
                                        uint32_t next_refresh) {
  // Caller is responsible for zeroing *cache (state_clear_arrival_cache).
  cache->next_refresh = next_refresh;
  cache->valid        = true;

  if (!data || len < 1) return;
  const uint8_t *p   = data;
  const uint8_t *end = data + len;

  uint8_t count = *p++;
  if (count > MAX_ARRIVALS) count = MAX_ARRIVALS;

  for (uint8_t i = 0; i < count; i++) {
    ArrivalEntry *e = &cache->entries[i];

    if (p + 3 > end) return;
    e->r = *p++;
    e->g = *p++;
    e->b = *p++;

    // lpStr: [u8 len][bytes] — return on truncated data, leaving previously
    // parsed entries intact (cache->count reflects fully-parsed entries only).
    #define LP(field) do { \
      if (p >= end) return; \
      uint8_t _l = *p++; \
      if (p + _l > end) return; \
      uint8_t _c = _l < sizeof(e->field) - 1 ? _l : sizeof(e->field) - 1; \
      memcpy(e->field, p, _c); e->field[_c] = 0; p += _l; \
    } while(0)

    LP(route);
    LP(headsign);

    // Raw numeric fields: u16 mins (big-endian) + s8 st
    if (p + 3 > end) return;
    e->mins = ((uint16_t)p[0] << 8) | p[1];
    e->st   = (int8_t)p[2];
    p += 3;

    #undef LP
    cache->count++;
  }
}
