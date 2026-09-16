/* Host shim for esp_wifi.h.
 *
 * netcfg.cpp re-initialises the radio to reserve its receive buffers rather
 * than borrow them from the application heap - the Arduino core hardcodes 4
 * static and 32 dynamic, and the dynamic ones are what a large frame map
 * starves. See WIFI_STATIC_RX_BUFFERS in config.h.
 *
 * None of that can happen on a host: there is no radio and no driver. What the
 * shim is for is making the CALL SITE compile and type-check, so a change to
 * the sequence is caught here rather than on the bench - the same reason every
 * other shim in this directory exists (where drifted
 * signatures hid real faults).
 *
 * The functions answer ESP_OK so the code takes its normal path; the counters
 * let a test see what was asked for.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef ESP_OK
#define ESP_OK 0
typedef int esp_err_t;
#endif

typedef struct {
  int static_rx_buf_num;
  int dynamic_rx_buf_num;
  int _reserved;
} wifi_init_config_t;

/* The real macro fills a large struct from Kconfig. Here only the two fields
 * the firmware touches need to exist, set to the values the Arduino core uses,
 * so a test can tell "left alone" from "deliberately changed". */
#define WIFI_INIT_CONFIG_DEFAULT() \
  wifi_init_config_t { 4, 32, 0 }

typedef enum { WIFI_STORAGE_FLASH = 0, WIFI_STORAGE_RAM = 1 } wifi_storage_t;

/* What the last esp_wifi_init() was asked for, so a host test can assert the
 * firmware requested the reservation rather than merely compiling the call. */
inline wifi_init_config_t &wifiShimLastInit() {
  static wifi_init_config_t c = { 0, 0, 0 };
  return c;
}
inline int &wifiShimInitCalls() { static int n = 0; return n; }

inline esp_err_t esp_wifi_stop(void) { return ESP_OK; }
inline esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
inline esp_err_t esp_wifi_set_storage(wifi_storage_t) { return ESP_OK; }

inline esp_err_t esp_wifi_init(const wifi_init_config_t *cfg) {
  if (cfg) wifiShimLastInit() = *cfg;
  wifiShimInitCalls()++;
  return ESP_OK;
}
