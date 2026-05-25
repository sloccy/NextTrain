#include "format.h"
#include <stdio.h>
#include <time.h>

uint16_t format_now_minute_of_day(void) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  return (uint16_t)(tm->tm_hour * 60 + tm->tm_min);
}

uint16_t format_arrival_predicted_min(uint16_t mins, int8_t st) {
  if (st <= -126) return mins;
  int16_t pred = (int16_t)mins + (int16_t)st;
  if (pred < 0)    pred += 1440;
  if (pred >= 1440) pred -= 1440;
  return (uint16_t)pred;
}

void format_wall_time(uint16_t mins, char *buf, size_t n) {
  uint16_t h = mins / 60;
  uint16_t m = mins % 60;
  if (clock_is_24h_style()) {
    snprintf(buf, n, "%u:%02u", (unsigned)h, (unsigned)m);
  } else {
    char period = (h >= 12) ? 'p' : 'a';
    h = h % 12;
    if (h == 0) h = 12;
    snprintf(buf, n, "%u:%02u%c", (unsigned)h, (unsigned)m, period);
  }
}

void format_countdown(uint16_t pred_mins, char *buf, size_t n) {
  int16_t now   = (int16_t)format_now_minute_of_day();
  int16_t delta = (int16_t)pred_mins - now;
  if (delta < -720) delta += 1440;
  if (delta >  720) delta -= 1440;

  if (delta <= -2) {
    snprintf(buf, n, "GONE");
  } else if (delta <= 1) {
    snprintf(buf, n, "NOW");
  } else if (delta < 60) {
    snprintf(buf, n, "%d MIN", (int)delta);
  } else if (delta < 120) {
    snprintf(buf, n, "%dh %dm", (int)(delta / 60), (int)(delta % 60));
  } else {
    snprintf(buf, n, "%dh+", (int)(delta / 60));
  }
}

void format_status_label(int8_t st, char *buf, size_t n,
                          GColor *out_color, bool *out_bold) {
  *out_bold = true;
  if (st == -128) {
    snprintf(buf, n, "CANCELED");
    *out_color = GColorRed;
  } else if (st == -127) {
    snprintf(buf, n, "SKIPPED");
    *out_color = GColorRed;
  } else if (st == -126) {
    snprintf(buf, n, "ON TIME");
    *out_color = GColorIslamicGreen;
  } else if (st == 0) {
    snprintf(buf, n, "SCHED.");
    *out_color = GColorBlack;
    *out_bold  = false;
  } else if (st > 0) {
    snprintf(buf, n, "+%d MIN", (int)st);
    *out_color = GColorOrange;
  } else {
    snprintf(buf, n, "-%d MIN", (int)(-st));
    *out_color = GColorIslamicGreen;
  }
}
