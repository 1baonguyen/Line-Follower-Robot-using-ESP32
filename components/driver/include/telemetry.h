#pragma once
#include "esp_err.h"

/*
 * Wireless log. Starts its own Wi-Fi network and sends every ESP_LOGx line over it (UDP).
 * Call once, AFTER ota_boot_window() (that creates the network stack, event loop and
 * Wi-Fi interface once, and leaves them in place).
 * If you never call it, Wi-Fi stays off and ADC2 (GPIO25) works normally.
 * Do not call ESP_LOGx from interrupt handlers (normal ESP-IDF rule).
 */
esp_err_t telemetry_init(void);

/* Send one line of your own data (e.g. a CSV row), without the log prefix.
 * Works like printf, end the line with \n. Does nothing before telemetry_init(). */
//void telemetry_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));