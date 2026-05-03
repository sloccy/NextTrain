#pragma once
#include <pebble.h>
#include "state.h"

typedef struct {
  char    station_slug[24];
  char    station_name[40];
  char    routes[64];       // "A:E,B:N"
  uint8_t query_index;      // favorite slot or QUERY_INDEX_TRANSIENT
  bool    from_favorite;    // false → show "Add to Favorites"
} ArrivalsParams;

void win_arrivals_push(const ArrivalsParams *params);
