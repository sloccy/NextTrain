#pragma once

#include <pebble.h>

// AppMessage key IDs — must match appinfo.json appKeys
typedef enum {
  MSG_OP               = 0,
  MSG_QUERY_STATION    = 1,
  MSG_QUERY_ROUTES     = 2,
  MSG_QUERY_INDEX      = 3,
  MSG_STATUS           = 4,
  MSG_DATA_TYPE        = 5,
  MSG_STATIONS_VERSION = 6,
  MSG_CHUNK_INDEX      = 7,
  MSG_CHUNK_TOTAL      = 8,
  MSG_PAYLOAD          = 9,
  MSG_STATION_NAME     = 10,
  MSG_NEXT_REFRESH     = 11,
} MessageKey;

// MSG_OP values (watch → phone)
typedef enum {
  OP_GET_STATIONS_VERSION = 1,
  OP_GET_STATIONS_FULL    = 2,
  OP_GET_ARRIVALS         = 3,
  OP_REFRESH_STATIONS     = 4,
} OpCode;

// MSG_DATA_TYPE values (phone → watch)
typedef enum {
  DATA_TYPE_STATIONS_VERSION = 1,
  DATA_TYPE_STATIONS_CHUNK   = 2,
  DATA_TYPE_ARRIVALS         = 3,
} DataType;

// MSG_STATUS values (phone → watch)
typedef enum {
  STATUS_OK       = 0,
  STATUS_OFFLINE  = 1,
  STATUS_NO_DATA  = 2,
  STATUS_ERROR    = 3,
} StatusCode;

// Arrival status codes inside the arrivals payload
typedef enum {
  ARRIVAL_LIVE      = 0,
  ARRIVAL_SCHEDULED = 1,
  ARRIVAL_CANCELED  = 2,
  ARRIVAL_SKIPPED   = 3,
  ARRIVAL_ADDED     = 4,
} ArrivalStatus;

// MSG_QUERY_INDEX sentinel for transient (search) queries
#define QUERY_INDEX_TRANSIENT 0xFF
