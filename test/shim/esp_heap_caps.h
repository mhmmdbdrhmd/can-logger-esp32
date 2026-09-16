#pragma once
/* The slice of esp_heap_caps.h that mem.cpp uses.
 *
 * The capability flags matter here and are not decoration. MALLOC_CAP_INTERNAL
 * means "internal RAM" and says nothing about byte addressability; on the ESP32
 * the leftover IRAM is donated to the heap as 32-BIT-ACCESS-ONLY, so a largest
 * free block reported under MALLOC_CAP_INTERNAL can be memory malloc() will
 * never hand to a String or an lwIP pbuf. Asking the wrong one cost two
 * sessions.
 *
 * The host values below are the ones this board REALLY reports at the point
 * the frame maps are loaded, not round numbers: a shim that answers 200000 to
 * every heap question cannot fail a budget test, and that is exactly why the
 * DBC_HEAP_RESERVE regression reached the bench. */
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  (1 << 12)

/* Measured on the board with no frame map loaded, MALLOC_CAP_8BIT, from
 * a boot log: free after Wi-Fi and before the maps. */
inline size_t heap_caps_get_free_size(uint32_t caps) {
  return (caps & MALLOC_CAP_8BIT) ? 76696u : 117696u;  /* +41 KB of IRAM */
}
inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return (caps & MALLOC_CAP_8BIT) ? 73716u : 47092u;   /* the IRAM lump */
}
inline size_t heap_caps_get_minimum_free_size(uint32_t caps) {
  return (caps & MALLOC_CAP_8BIT) ? 71988u : 112988u;
}

/* ---- the failed-allocation hook ---------------------------------------- *
 *
 * On the board this fires on every allocation the heap could not satisfy, and
 * it is the instrument that separates "lwIP could not get memory for the
 * inbound SYN" from every other explanation for a refused connection. On the
 * host there is nothing to fail, so registering it succeeds and the hook is
 * never called - which is exactly what the tests need: the registration path
 * compiles and is type-checked, and no test can accidentally depend on a
 * failure that only a real heap can produce. */
typedef int esp_err_t;
#define ESP_OK 0

typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps,
                                        const char *function_name);

inline esp_err_t heap_caps_register_failed_alloc_callback(
    esp_alloc_failed_hook_t cb) {
  (void)cb;
  return ESP_OK;
}
