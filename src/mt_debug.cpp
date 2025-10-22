#include "mt_internals.h"
#include <Arduino.h>
#include <stdarg.h>

static const uint32_t DEBUG_RATE_MS = 50; // ms
static uint32_t last_debug_ms = 0;

void _d(const char * fmt, ...) {
  uint32_t now = millis();
  if (now - last_debug_ms < DEBUG_RATE_MS) return;
  last_debug_ms = now;

  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Serial.println(buf);
}
