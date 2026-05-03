#pragma once
#include <pebble.h>
#include "state.h"

// Station picker: push a scrollable list of all stations.
// On selection, immediately pushes the route picker for that station.
void win_station_picker_push(void);

// Route picker: push a multi-select list of routes for the given station slug.
// On confirm, pushes win_arrivals with the selection.
void win_route_picker_push(const char *station_slug, const char *station_name);
