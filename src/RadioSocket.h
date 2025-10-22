#pragma once
#include <stddef.h>

struct RadioSocket {
  virtual bool connected() = 0;
  virtual int available() = 0;
  virtual int read() = 0; // returns -1 if none
  virtual size_t write(const char* buf, size_t len) = 0;
  virtual void stop() = 0;
  virtual ~RadioSocket() {}
};
