#pragma once

#include <pebble.h>

// Current time as minute-of-day (0..1439).
uint16_t format_now_minute_of_day(void);

// Predicted departure minute: mins + st for live/delta statuses,
// mins unchanged for sentinel codes (st <= -126).
uint16_t format_arrival_predicted_min(uint16_t mins, int8_t st);

// Format minute-of-day as wall-clock string.
// 12h: "5:42p" / 24h: "17:42". buf must be at least 8 bytes.
void format_wall_time(uint16_t mins, char *buf, size_t n);

// Format predicted minute as a countdown string relative to now.
// Outputs: "GONE", "NOW", "N MIN", "Nh Mm", "Nh+". buf must be at least 10 bytes.
void format_countdown(uint16_t pred_mins, char *buf, size_t n);

// Map raw status int8 to a short label, semantic GColor, and bold flag.
// buf must be at least 12 bytes.
void format_status_label(int8_t st, char *buf, size_t n,
                         GColor *out_color, bool *out_bold);
