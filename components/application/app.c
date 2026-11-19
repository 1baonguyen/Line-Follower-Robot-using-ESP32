/* app.c */

#include "app.h"
#include "ota.h"
#include "wifi_ap.h"
#include "telemetry.h"
#include "loadcell_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static const char *TAG = "app";

void app_start(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    ESP_LOGW("BOOT", "================================");
    ESP_LOGW("BOOT", "ESP32 RESTART DETECTED");
    ESP_LOGW("BOOT", "Reset reason: %d", reason);
    ESP_LOGW("BOOT", "================================");
    
    /* 1. Start log capture immediately (queues logs until Wi-Fi is up) */
    wifi_ap_init_base();          /* 1. Start lwIP + event loop once */
    telemetry_init();   

    ESP_LOGI(TAG, "Starting up");
    
    /* 2. Open OTA window (turns on LineRobot-OTA; boot logs broadcast here!) */
    ota_boot_window();


     /* 3. Normal start-up */
    if (loadcell_service_init() != ESP_OK) {
        ESP_LOGE(TAG, "Load cell init failed");
        return;
    }

    /* Zero the scale. The load cell must be empty right now. */
    ESP_LOGI(TAG, "Taring in 2 s - keep the load cell EMPTY");
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (loadcell_service_tare() != ESP_OK) {
        ESP_LOGE(TAG, "No answer from HX711 - check DOUT/SCK wiring and 3V3 power");
        return;
    }
    ESP_LOGI(TAG, "Tare done, offset = %ld", (long)loadcell_service_get_offset());

    /* Main loop: read the load cell and print it. Later: sensors, motors, line following. */
    while (1) {
        loadcell_reading_t r;

        if (loadcell_service_read(&r) == ESP_OK) {
            ESP_LOGI(TAG, "raw=%ld  net=%ld  grams=%.2f",
                     (long)r.raw, (long)r.net, r.grams);
        } else {
            ESP_LOGW(TAG, "Load cell read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
