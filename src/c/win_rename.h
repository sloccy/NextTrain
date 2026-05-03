#pragma once

#include <pebble.h>

typedef void (*RenameCompleteCb)(void);

// Open a dictation session to rename the favorite at index. On success, calls
// state_set_favorite_name and then cb. On cancel/error, calls cb unchanged.
void win_rename_start(uint8_t favorite_index, RenameCompleteCb cb);
