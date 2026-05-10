#pragma once

#include <pebble.h>
#include "protocol.h"

#define MAX_FAVORITES          16
#define MAX_ARRIVALS           10
#define MAX_STATIONS           100
#define MAX_ROUTES_PER_STATION 12
#define MAX_FAV_ROUTES          8
#define FAVORITE_NAME_LEN      24

// ─── Stations cache ──────────────────────────────────────────────────────────

typedef struct {
  uint8_t r, g, b;
  char    route[4];     // null-terminated, e.g. "A"
  char    dir;          // 'N','S','E','W'
  char    headsign[25];
} StationRoute;

typedef struct {
  char          slug[40];
  uint8_t       route_count;
  StationRoute *routes;   // points into StationsCache.route_pool
} Station;

typedef struct {
  uint32_t      generated_at;
  uint16_t      station_count;
  Station      *stations;
  StationRoute *route_pool;
  bool          valid;
  // True after a full chunked sync; false when only the favorites-only persist
  // subset is loaded. The picker requires the full list — the subset is just
  // for fast home-screen icon rendering on cold start.
  bool          is_full;
} StationsCache;

// ─── Arrivals cache (per-favorite slot + one transient slot) ─────────────────

typedef struct {
  uint8_t       r, g, b;
  char          route[4];
  char          headsign[25];
  uint16_t      mins;  // backend minute-of-day (0..1439)
  int8_t        st;    // status sentinel/delta; see format_status_label()
} ArrivalEntry;

typedef struct {
  bool         valid;
  uint8_t      count;
  ArrivalEntry entries[MAX_ARRIVALS];
  uint32_t     next_refresh;
} ArrivalCache;

// ─── Favorites ───────────────────────────────────────────────────────────────

typedef struct {
  char    station_slug[40];
  uint8_t route_count;
  struct {
    char route[4]; // null-terminated
    char dir;      // 'N','S','E','W'
  } routes[MAX_FAV_ROUTES];
  char name[FAVORITE_NAME_LEN]; // user-visible nickname; "" falls back to slug_to_display
} Favorite;

// ─── Persistent storage key allocation ───────────────────────────────────────
// Key 0       : STATIONS_VERSION (uint32)
// Key 1       : STATIONS_BLOB_SIZE (uint32) — total byte length of assembled blob
// Key 2..101  : STATIONS_CHUNK_i (256-byte slices of the blob)
// Key 100     : SCHEMA_VERSION (uint8) — bumped when Favorite binary layout changes
// Key 200     : FAVORITES_COUNT (uint8)
// Key 201..216: FAVORITE_i (one Favorite struct each)

#define PERSIST_KEY_STATIONS_VERSION    0
#define PERSIST_KEY_STATIONS_BLOB_SIZE  1
#define PERSIST_KEY_STATIONS_CHUNK_BASE 2   // keys 2..101
#define PERSIST_KEY_SCHEMA_VERSION      100
#define PERSIST_KEY_FAVORITES_COUNT     200
#define PERSIST_KEY_FAVORITES_BASE      201 // keys 201..216

#define SCHEMA_V_NAMES_DROPPED 1
#define SCHEMA_V_USER_NAMES    2

// ─── API ─────────────────────────────────────────────────────────────────────

void state_init(void);
void state_deinit(void);

// Stations
StationsCache *state_get_stations(void);
uint32_t       state_get_persisted_stations_version(void);
bool           state_load_stations_from_buffer(const uint8_t *blob, uint32_t size);
bool           state_load_stations_from_persist(void);
void           state_persist_favorite_stations(void);
void           state_free_stations(void);

// Favorites
uint8_t    state_get_favorite_count(void);
Favorite  *state_get_favorite(uint8_t index);
void       state_add_favorite(const Favorite *fav);
void       state_remove_favorite(uint8_t index);
void       state_swap_favorites(uint8_t a, uint8_t b);
void       state_set_favorite_name(uint8_t index, const char *name);

// Arrivals cache
// index: 0..(MAX_FAVORITES-1) for saved slots, QUERY_INDEX_TRANSIENT for search
ArrivalCache *state_get_arrival_cache(uint8_t index);
void          state_set_arrival_cache(uint8_t index, const ArrivalCache *cache);
void          state_clear_arrival_cache(uint8_t index);

// Helpers
const Station *state_find_station(const char *slug);

// Encode Favorite routes as a comma-separated "route:dir" query string (e.g. "A:N,B:E")
void state_format_routes_query(const Favorite *fav, char *buf, size_t buf_size);

// Returns the index of the first favorite matching slug + routes_query, or -1 if none.
int8_t state_find_favorite_by_slug_and_routes(const char *slug, const char *routes_query);

// Convert slug "union_station" → "Union Station". Underscores become spaces;
// first char of each word uppercased. Mechanical — no punctuation reconstruction.
void slug_to_display(const char *slug, char *out, size_t n);
