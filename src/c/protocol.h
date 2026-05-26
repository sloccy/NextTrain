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
  OP_GET_ALERTS_SUMMARY   = 5,
  OP_GET_ALERT_DETAIL     = 6,
} OpCode;

// MSG_DATA_TYPE values (phone → watch)
typedef enum {
  DATA_TYPE_STATIONS_VERSION   = 1,
  DATA_TYPE_STATIONS_CHUNK     = 2,
  DATA_TYPE_ARRIVALS           = 3,
  DATA_TYPE_ALERTS_SUMMARY     = 4,
  DATA_TYPE_ALERT_DETAIL       = 5,
  DATA_TYPE_FAVORITES_REQUEST  = 6, // phone → watch: request favorites list for config
  DATA_TYPE_FAVORITES_LIST     = 7, // watch → phone: JSON array of current favorites
  DATA_TYPE_RENAME_FAVORITE    = 8, // phone → watch: rename a specific favorite
  DATA_TYPE_ALERT_DETAIL_CHUNK = 9, // phone → watch: chunked alert detail payload
} DataType;

// MSG_STATUS values (phone → watch)
typedef enum {
  STATUS_OK       = 0,
  STATUS_OFFLINE  = 1,
  STATUS_NO_DATA  = 2,
  STATUS_ERROR    = 3,
} CommStatus;

// MSG_QUERY_INDEX sentinel for transient (search) queries
#define QUERY_INDEX_TRANSIENT 0xFF
