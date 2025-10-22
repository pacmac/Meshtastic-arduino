#ifdef MT_WIFI_SUPPORTED

#include <WiFi.h>
#include "RadioSocket.h"
#include "mt_internals.h"
#include <Arduino.h>

#define CONNECT_TIMEOUT (10 * 1000)
#define IDLE_TIMEOUT (65 * 1000)

#define RADIO_IP "192.168.42.1"
#define RADIO_PORT 4403

#define UNUSED_WIFI_STATUS 254

static uint8_t last_wifi_status = UNUSED_WIFI_STATUS;
static uint32_t next_connect_attempt = 0;

RadioSocket* mt_radio_socket = nullptr;
void mt_wifi_set_socket(RadioSocket* s) { mt_radio_socket = s; }

static const char* ssid_g = nullptr;
static const char* password_g = nullptr;

bool can_send = false;

void mt_wifi_init(int8_t cs_pin, int8_t irq_pin, int8_t reset_pin,
    int8_t enable_pin, const char * ssid_, const char * password_) {
  ssid_g = ssid_;
  password_g = password_;
  next_connect_attempt = 0;
  last_wifi_status = UNUSED_WIFI_STATUS;
  can_send = false;
  mt_wifi_mode = true;
  mt_serial_mode = false;
}

void print_wifi_status() {
#ifdef MT_DEBUGGING
  IPAddress ip = WiFi.localIP();
  d("IP Address: %s", ip.toString().c_str());
  long rssi = WiFi.RSSI();
  d("Signal strength (RSSI): %ld dBm", rssi);
#endif
}

bool open_tcp_connection() {
  if (!mt_radio_socket) {
    d("No radio socket set");
    return false;
  }
  can_send = mt_radio_socket->connect(RADIO_IP, RADIO_PORT);
  if (can_send) d("TCP connection established");
  else d("Failed to establish TCP connection");
  return can_send;
}

bool mt_wifi_loop(uint32_t now) {
  uint8_t wifi_status = WiFi.status();

  if (now >= next_connect_attempt) {
    last_wifi_status = UNUSED_WIFI_STATUS;
    wifi_status = WL_IDLE_STATUS;
  }

  if (wifi_status == last_wifi_status) return can_send;
  last_wifi_status = wifi_status;

  switch (wifi_status) {
    case WL_NO_SHIELD:
      d("No WiFi shield detected");
      while(true);
    case WL_CONNECT_FAILED:
    case WL_CONNECTION_LOST:
    case WL_IDLE_STATUS:
      next_connect_attempt = now + CONNECT_TIMEOUT;
      d("Attempting to connect to WiFi...");
      if (ssid_g == NULL) {
        d("No SSID provided");
      } else {
        if (password_g == NULL) WiFi.begin(ssid_g);
        else WiFi.begin(ssid_g, password_g);
      }
      can_send = false;
      return false;
    case WL_CONNECTED:
#ifdef MT_DEBUGGING
      print_wifi_status();
#endif
      mt_wifi_reset_idle_timeout(now);
      open_tcp_connection();
      return can_send;
    case WL_DISCONNECTED:
      can_send = false;
      return false;
    default:
#ifdef MT_DEBUGGING
      d("Unknown WiFi status %u", wifi_status);
#endif
      while(true);
  }
}

size_t mt_wifi_check_radio(char * buf, size_t space_left) {
  if (!mt_radio_socket || !mt_radio_socket->connected()) {
    d("Lost TCP connection");
    return 0;
  }
  size_t bytes_read = 0;
  while (mt_radio_socket->available()) {
    int rc = mt_radio_socket->read();
    if (rc < 0) break;
    char c = (char)rc;
    *buf++ = c;
    if (++bytes_read >= space_left) {
      d("TCP overflow");
      mt_radio_socket->stop();
      break;
    }
  }
  return bytes_read;
}

bool mt_wifi_send_radio(const char * buf, size_t len) {
  if (!mt_radio_socket || !mt_radio_socket->connected()) {
    d("Lost TCP connection? Attempting to reconnect...");
    if (!open_tcp_connection()) return false;
  }
  size_t wrote = mt_radio_socket->write(buf, len);
  if (wrote == len) return true;
  d("Tried to send radio %u but actually sent %u", (unsigned)len, (unsigned)wrote);
  mt_radio_socket->stop();
  return false;
}

void mt_wifi_reset_idle_timeout(uint32_t now) {
  next_connect_attempt = now + IDLE_TIMEOUT;
}

#endif
