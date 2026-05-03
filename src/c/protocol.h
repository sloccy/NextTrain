#pragma once

#include <pebble.h>
// AppMessage key IDs — auto-generated from appinfo.json appKeys.
// Provides MESSAGE_KEY_OP, MESSAGE_KEY_STATUS, MESSAGE_KEY_DATA_TYPE, etc.
#include "message_keys.auto.h"

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
} CommStatus;

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
