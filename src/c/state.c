#include "state.h"
#include <stdlib.h>
#include <string.h>

static bool prv_parse_blob(const uint8_t *data, uint32_t size);

#define CHUNK_SIZE 256

// Persist quota is ~4KB per app. Favorites (≤16 × 105B) consume ~1.7KB plus
// per-key metadata; this leaves ~2KB safely available for the stations subset.
#define FAVORITE_SUBSET_MAX_SIZE 2048

// ─── Module-level singletons ─────────────────────────────────────────────────

static StationsCache s_stations = {0};
static Favorite      s_favorites[MAX_FAVORITES];
static uint8_t       s_favorite_count = 0;

// MAX_FAVORITES slots + 1 transient slot (index 0xFF → mapped to slot MAX_FAVORITES)
static ArrivalCache s_arrival_cache[MAX_FAVORITES + 1];

static inline uint8_t cache_idx(uint8_t index) {
  return (index == QUERY_INDEX_TRANSIENT) ? MAX_FAVORITES : index;
}

// ─── Init / Deinit ───────────────────────────────────────────────────────────

void state_init(void) {
  memset(&s_stations, 0, sizeof(s_stations));
  memset(s_favorites, 0, sizeof(s_favorites));
  memset(s_arrival_cache, 0, sizeof(s_arrival_cache));
  s_favorite_count = 0;

  // Schema migration: Favorite lost station_name[40]. Read old layout, write new.
  uint8_t schema = persist_exists(PERSIST_KEY_SCHEMA_VERSION)
      ? (uint8_t)persist_read_int(PERSIST_KEY_SCHEMA_VERSION) : 0;

  if (schema < SCHEMA_V_NAMES_DROPPED) {
    typedef struct {
      char    station_slug[40];
      char    station_name[40]; // dropped in SCHEMA_V_NAMES_DROPPED
      uint8_t route_count;
      struct { char route[4]; char dir; } routes[MAX_FAV_ROUTES];
    } FavoriteV0;

    uint8_t count = persist_exists(PERSIST_KEY_FAVORITES_COUNT)
        ? (uint8_t)persist_read_int(PERSIST_KEY_FAVORITES_COUNT) : 0;
    if (count > MAX_FAVORITES) count = MAX_FAVORITES;

    for (uint8_t i = 0; i < count; i++) {
      FavoriteV0 old;
      memset(&old, 0, sizeof(old));
      persist_read_data(PERSIST_KEY_FAVORITES_BASE + i, &old, sizeof(old));

      Favorite nw;
      memset(&nw, 0, sizeof(nw));
      strncpy(nw.station_slug, old.station_slug, sizeof(nw.station_slug) - 1);
      nw.route_count = old.route_count;
      for (uint8_t r = 0; r < nw.route_count && r < MAX_FAV_ROUTES; r++) {
        strncpy(nw.routes[r].route, old.routes[r].route, sizeof(nw.routes[r].route) - 1);
        nw.routes[r].dir = old.routes[r].dir;
      }
      persist_write_data(PERSIST_KEY_FAVORITES_BASE + i, &nw, sizeof(Favorite));
    }

    // Wipe the stations subset — it was encoded with the old wire format (had
    // embedded names) and would be mis-parsed by the updated parser. Force a
    // clean re-sync from the worker on next boot.
    persist_write_int(PERSIST_KEY_STATIONS_VERSION, 0);
    persist_delete(PERSIST_KEY_STATIONS_BLOB_SIZE);
    for (int k = 0; k < 100; k++) {
      persist_delete(PERSIST_KEY_STATIONS_CHUNK_BASE + k);
    }

    persist_write_int(PERSIST_KEY_SCHEMA_VERSION, SCHEMA_V_NAMES_DROPPED);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "[state] schema migrated v0→v%d: %u favorites rewritten, stations cache wiped",
            SCHEMA_V_NAMES_DROPPED, (unsigned)count);
  }

  // Load favorites from persist
  s_favorite_count = persist_exists(PERSIST_KEY_FAVORITES_COUNT)
      ? (uint8_t)persist_read_int(PERSIST_KEY_FAVORITES_COUNT) : 0;
  if (s_favorite_count > MAX_FAVORITES) s_favorite_count = MAX_FAVORITES;

  for (uint8_t i = 0; i < s_favorite_count; i++) {
    persist_read_data(PERSIST_KEY_FAVORITES_BASE + i, &s_favorites[i], sizeof(Favorite));
  }
}

void state_deinit(void) {
  state_free_stations();
}

// ─── Stations ────────────────────────────────────────────────────────────────

void state_free_stations(void) {
  if (s_stations.stations)  { free(s_stations.stations);  s_stations.stations  = NULL; }
  if (s_stations.route_pool) { free(s_stations.route_pool); s_stations.route_pool = NULL; }
  s_stations.valid = false;
  s_stations.is_full = false;
}

StationsCache *state_get_stations(void) {
  return s_stations.valid ? &s_stations : NULL;
}

uint32_t state_get_persisted_stations_version(void) {
  return persist_exists(PERSIST_KEY_STATIONS_VERSION)
      ? (uint32_t)persist_read_int(PERSIST_KEY_STATIONS_VERSION) : 0;
}

bool state_load_stations_from_buffer(const uint8_t *blob, uint32_t size) {
  bool ok = prv_parse_blob(blob, size);
  if (ok) s_stations.is_full = true;
  return ok;
}

bool state_load_stations_from_persist(void) {
  if (!persist_exists(PERSIST_KEY_STATIONS_VERSION)) return false;
  if (!persist_exists(PERSIST_KEY_STATIONS_BLOB_SIZE)) return false;

  uint32_t size = (uint32_t)persist_read_int(PERSIST_KEY_STATIONS_BLOB_SIZE);
  if (size == 0 || size > FAVORITE_SUBSET_MAX_SIZE) return false;

  uint8_t *blob = malloc(size);
  if (!blob) return false;

  uint32_t offset = 0;
  for (int key = PERSIST_KEY_STATIONS_CHUNK_BASE; offset < size; key++) {
    uint32_t chunk = size - offset;
    if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
    int read = persist_read_data(key, blob + offset, (size_t)chunk);
    if (read < 0 || (uint32_t)read != chunk) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "[state] persist_read_data short/failed key=%d expected=%lu got=%d",
              key, (unsigned long)chunk, read);
      free(blob);
      return false;
    }
    offset += (uint32_t)read;
  }

  bool ok = prv_parse_blob(blob, size);
  free(blob);
  return ok;
}

// Build a subset blob containing only stations referenced by current favorites,
// in the same wire format as the full blob (so prv_parse_blob handles both).
// The full 9KB+ stations blob exceeds Pebble's 4KB per-app persist quota, so
// we persist only the user-visible subset — "as many favorite stations as fit."
void state_persist_favorite_stations(void) {
  if (!s_stations.valid) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "[state] persist_favorites: stations cache invalid");
    return;
  }

  uint8_t *buf = malloc(FAVORITE_SUBSET_MAX_SIZE);
  if (!buf) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "[state] persist_favorites: malloc failed");
    return;
  }

  uint32_t off = 0;

  // Header: u32 generated_at + u16 station_count (count patched at end)
  uint32_t g = s_stations.generated_at;
  buf[off++] = (g >> 24) & 0xFF;
  buf[off++] = (g >> 16) & 0xFF;
  buf[off++] = (g >>  8) & 0xFF;
  buf[off++] =  g        & 0xFF;
  uint32_t count_off = off;
  buf[off++] = 0; buf[off++] = 0;

  uint16_t count = 0;
  const char *seen[MAX_FAVORITES];
  uint8_t seen_count = 0;

  for (uint8_t f = 0; f < s_favorite_count; f++) {
    const char *slug = s_favorites[f].station_slug;

    bool dup = false;
    for (uint8_t i = 0; i < seen_count; i++) {
      if (strcmp(seen[i], slug) == 0) { dup = true; break; }
    }
    if (dup) continue;
    seen[seen_count++] = slug;

    const Station *st = state_find_station(slug);
    if (!st) continue;

    // Compute encoded size for this station up-front so we can stop cleanly
    uint32_t need = 1 + strlen(st->slug)
                  + 1; // route_count
    for (uint8_t r = 0; r < st->route_count; r++) {
      need += 3                              // color rgb
            + 1 + strlen(st->routes[r].route)
            + 1                              // dir
            + 1 + strlen(st->routes[r].headsign);
    }

    if (off + need > FAVORITE_SUBSET_MAX_SIZE) {
      APP_LOG(APP_LOG_LEVEL_INFO,
              "[state] subset full at %u stations, skipping rest", (unsigned)count);
      break;
    }

    uint8_t sl = (uint8_t)strlen(st->slug);
    buf[off++] = sl; memcpy(buf + off, st->slug, sl); off += sl;

    buf[off++] = st->route_count;

    for (uint8_t r = 0; r < st->route_count; r++) {
      const StationRoute *rt = &st->routes[r];
      buf[off++] = rt->r; buf[off++] = rt->g; buf[off++] = rt->b;

      uint8_t rl = (uint8_t)strlen(rt->route);
      buf[off++] = rl; memcpy(buf + off, rt->route, rl); off += rl;

      buf[off++] = (uint8_t)rt->dir;

      uint8_t hl = (uint8_t)strlen(rt->headsign);
      buf[off++] = hl; memcpy(buf + off, rt->headsign, hl); off += hl;
    }

    count++;
  }

  buf[count_off    ] = (count >> 8) & 0xFF;
  buf[count_off + 1] =  count       & 0xFF;

  // Persist: clear version first so a partial write isn't readable on next boot
  persist_write_int(PERSIST_KEY_STATIONS_VERSION, 0);
  persist_write_int(PERSIST_KEY_STATIONS_BLOB_SIZE, (int)off);

  uint32_t key_off = 0;
  for (int key = PERSIST_KEY_STATIONS_CHUNK_BASE; key_off < off; key++) {
    uint32_t chunk = off - key_off;
    if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
    status_t s = persist_write_data(key, buf + key_off, (size_t)chunk);
    if (s < 0) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "[state] persist_write_data key=%d failed: %d", key, (int)s);
      free(buf);
      return; // version stays 0 → next load fails → triggers full re-sync
    }
    key_off += chunk;
  }

  persist_write_int(PERSIST_KEY_STATIONS_VERSION, (int)g);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "[state] persisted %u favorite stations (%lu bytes)",
          (unsigned)count, (unsigned long)off);

  free(buf);
}

// ─── Blob parser ─────────────────────────────────────────────────────────────

static bool prv_parse_blob(const uint8_t *data, uint32_t size) {
  // All locals declared at top — avoids C99 jump-over-declaration UB
  const uint8_t *p   = data;
  const uint8_t *end = data + size;
  uint32_t max_routes = 0;
  uint32_t pool_idx   = 0;
  uint16_t i;
  uint8_t  j, sl, rl, hl, copy;

  #define NEED(n) if (p + (n) > end) goto fail

  state_free_stations();

  NEED(6);
  s_stations.generated_at  = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                            | ((uint32_t)p[2] << 8)  | p[3];
  p += 4;
  s_stations.station_count = ((uint16_t)p[0] << 8) | p[1];
  p += 2;

  if (s_stations.station_count == 0) { s_stations.valid = true; return true; }

  s_stations.stations = malloc(s_stations.station_count * sizeof(Station));
  if (!s_stations.stations) goto fail;

  max_routes = (uint32_t)s_stations.station_count * MAX_ROUTES_PER_STATION;
  s_stations.route_pool = malloc(max_routes * sizeof(StationRoute));
  if (!s_stations.route_pool) goto fail;

  for (i = 0; i < s_stations.station_count; i++) {
    Station *st = &s_stations.stations[i];

    NEED(1); sl = *p++;
    NEED(sl); copy = sl < sizeof(st->slug) - 1 ? sl : sizeof(st->slug) - 1;
    memcpy(st->slug, p, copy); st->slug[copy] = 0; p += sl;

    NEED(1); st->route_count = *p++;
    st->routes = &s_stations.route_pool[pool_idx];

    for (j = 0; j < st->route_count; j++) {
      StationRoute *rt;
      if (pool_idx >= max_routes) goto fail;
      rt = &s_stations.route_pool[pool_idx++];

      NEED(3); rt->r = *p++; rt->g = *p++; rt->b = *p++;

      NEED(1); rl = *p++;
      NEED(rl); copy = rl < sizeof(rt->route) - 1 ? rl : sizeof(rt->route) - 1;
      memcpy(rt->route, p, copy); rt->route[copy] = 0; p += rl;

      NEED(1); rt->dir = (char)*p++;

      NEED(1); hl = *p++;
      NEED(hl); copy = hl < sizeof(rt->headsign) - 1 ? hl : sizeof(rt->headsign) - 1;
      memcpy(rt->headsign, p, copy); rt->headsign[copy] = 0; p += hl;
    }
  }

  s_stations.valid = true;
  #undef NEED
  return true;

fail:
  state_free_stations();
  #undef NEED
  return false;
}

// ─── Favorites ───────────────────────────────────────────────────────────────

uint8_t state_get_favorite_count(void) { return s_favorite_count; }

Favorite *state_get_favorite(uint8_t index) {
  if (index >= s_favorite_count) return NULL;
  return &s_favorites[index];
}

void state_add_favorite(const Favorite *fav) {
  if (s_favorite_count >= MAX_FAVORITES) return;
  s_favorites[s_favorite_count] = *fav;
  persist_write_data(PERSIST_KEY_FAVORITES_BASE + s_favorite_count,
                     fav, sizeof(Favorite));
  s_favorite_count++;
  persist_write_int(PERSIST_KEY_FAVORITES_COUNT, s_favorite_count);
  state_persist_favorite_stations();
}

void state_remove_favorite(uint8_t index) {
  if (index >= s_favorite_count) return;
  state_clear_arrival_cache(index);
  for (uint8_t i = index; i < s_favorite_count - 1; i++) {
    s_favorites[i] = s_favorites[i + 1];
    persist_write_data(PERSIST_KEY_FAVORITES_BASE + i,
                       &s_favorites[i], sizeof(Favorite));
    // Shift arrival caches too
    s_arrival_cache[i] = s_arrival_cache[i + 1];
  }
  s_favorite_count--;
  persist_write_int(PERSIST_KEY_FAVORITES_COUNT, s_favorite_count);
  // Clear the now-invalid last slot in persist
  persist_delete(PERSIST_KEY_FAVORITES_BASE + s_favorite_count);
  state_persist_favorite_stations();
}

void state_swap_favorites(uint8_t a, uint8_t b) {
  if (a >= s_favorite_count || b >= s_favorite_count || a == b) return;
  Favorite tmp = s_favorites[a];
  s_favorites[a] = s_favorites[b];
  s_favorites[b] = tmp;
  persist_write_data(PERSIST_KEY_FAVORITES_BASE + a, &s_favorites[a], sizeof(Favorite));
  persist_write_data(PERSIST_KEY_FAVORITES_BASE + b, &s_favorites[b], sizeof(Favorite));
  // Swap arrival caches
  ArrivalCache tmp_cache = s_arrival_cache[a];
  s_arrival_cache[a] = s_arrival_cache[b];
  s_arrival_cache[b] = tmp_cache;
}

// ─── Arrivals cache ───────────────────────────────────────────────────────────

ArrivalCache *state_get_arrival_cache(uint8_t index) {
  return &s_arrival_cache[cache_idx(index)];
}

void state_set_arrival_cache(uint8_t index, const ArrivalCache *cache) {
  s_arrival_cache[cache_idx(index)] = *cache;
}

void state_clear_arrival_cache(uint8_t index) {
  memset(&s_arrival_cache[cache_idx(index)], 0, sizeof(ArrivalCache));
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

const Station *state_find_station(const char *slug) {
  if (!s_stations.valid) return NULL;
  for (uint16_t i = 0; i < s_stations.station_count; i++) {
    if (strcmp(s_stations.stations[i].slug, slug) == 0) {
      return &s_stations.stations[i];
    }
  }
  return NULL;
}

void state_format_routes_query(const Favorite *fav, char *buf, size_t buf_size) {
  if (!fav || buf_size == 0) return;
  buf[0] = '\0';
  for (uint8_t i = 0; i < fav->route_count; i++) {
    char part[8];
    snprintf(part, sizeof(part), "%s:%c", fav->routes[i].route, fav->routes[i].dir);
    if (i > 0) strncat(buf, ",", buf_size - strlen(buf) - 1);
    strncat(buf, part, buf_size - strlen(buf) - 1);
  }
}

void slug_to_display(const char *slug, char *out, size_t n) {
  if (n == 0) return;
  size_t o = 0;
  bool word_start = true;
  for (size_t i = 0; slug[i] && o + 1 < n; i++) {
    char c = slug[i];
    if (c == '_') { out[o++] = ' '; word_start = true; continue; }
    if (word_start && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out[o++] = c;
    word_start = false;
  }
  out[o] = '\0';
}
