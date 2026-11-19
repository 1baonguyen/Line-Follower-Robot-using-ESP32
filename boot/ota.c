#include <stdbool.h>
#include <string.h>
#include "ota.h"
#include "wifi_ap.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static volatile bool s_busy = false;     /* true while a file is being uploaded */

static const char PAGE[] =
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Robot update</title></head>"
"<body style='font-family:sans-serif;max-width:420px;margin:2em auto'>"
"<h2>Robot firmware update</h2>"
"<input type='file' id='f' accept='.bin'><br><br>"
"<button onclick='up()'>Upload</button><p id='s'></p>"
"<script>"
"function up(){var f=document.getElementById('f').files[0],s=document.getElementById('s');"
"if(!f){s.innerText='Choose a .bin file first';return;}"
"var x=new XMLHttpRequest();x.open('POST','/update');"
"x.upload.onprogress=function(e){if(e.lengthComputable)s.innerText='Uploading '+Math.round(e.loaded*100/e.total)+'%';};"
"x.onload=function(){s.innerText=x.responseText;};"
"x.onerror=function(){s.innerText='Connection lost (the robot may be rebooting)';};"
"x.send(f);}"
"</script></body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t handle, const char *msg)
{
    ESP_LOGE(TAG, "%s", msg);
    if (handle) {
        esp_ota_abort(handle);
    }
    s_busy = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
}

/* Writes the uploaded .bin into the spare app slot, selects it, reboots. */
static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL) {
        return fail(req, 0, "No spare app slot - check the partition table setting");
    }
    if (req->content_len == 0) {
        return fail(req, 0, "Empty file");
    }

    s_busy = true;

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(slot, req->content_len, &handle) != ESP_OK) {
        return fail(req, 0, "Could not start the update");
    }

    char buf[1024];
    int remaining = (int)req->content_len;

    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return fail(req, handle, "Upload interrupted");
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            return fail(req, handle, "Flash write failed");
        }
        remaining -= n;
    }

    if (esp_ota_end(handle) != ESP_OK) {            /* checks it is valid firmware */
        return fail(req, 0, "Not a valid firmware file");
    }
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        return fail(req, 0, "Could not select the new firmware");
    }

    ESP_LOGI(TAG, "Update OK, rebooting");
    httpd_resp_sendstr(req, "Done! The robot is rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// static bool wifi_ap_start(void)
// {
//     /* . Init flash storage for Wifi radio calib*/

//     esp_err_t err = nvs_flash_init();
//     if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         nvs_flash_erase();
//         err = nvs_flash_init();
//     }
//     if (err != ESP_OK) return false;

//     /* Layer 3 - Network: Initializes the lwIP TCP/IP stack,
//     start the event loop,
//     assigns the ESP32 192.168.4.1, and launches the DHCP server to hand out 192.168.4.2 to PC.*/

//     if (esp_netif_init() != ESP_OK) return false;
//     if (esp_event_loop_create_default() != ESP_OK) return false;
//     esp_netif_create_default_wifi_ap();

//     /* Layers 1 & 2 — Physical & Data Link: Allocates the Wi-Fi driver, 
//     configures the SSID/WPA2 password, and turns on the 2.4 GHz radio antenna in Access Point mode.*/
//     wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
//     if (esp_wifi_init(&init) != ESP_OK) return false;

//     wifi_config_t ap = { 0 };
//     strncpy((char *)ap.ap.ssid, OTA_WIFI_SSID, sizeof(ap.ap.ssid) - 1);
//     strncpy((char *)ap.ap.password, OTA_WIFI_PASS, sizeof(ap.ap.password) - 1);
//     ap.ap.ssid_len       = strlen(OTA_WIFI_SSID);
//     ap.ap.channel        = 1;
//     ap.ap.max_connection = 2;
//     ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;

//     if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) return false;
//     if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;
//     return esp_wifi_start() == ESP_OK;
// }

void ota_boot_window(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_USER_LED,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);

    /* Start Wi-Fi Layers 1-3 via shared module */
    if (wifi_ap_on(OTA_WIFI_SSID, OTA_WIFI_PASS) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi failed to start, skipping OTA window");
        return;
    }

    /* Layers 4 & 5: HTTP server */
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start, skipping OTA window");
        wifi_ap_off();
        return;
    }

    httpd_uri_t page   = { .uri = "/",       .method = HTTP_GET,  .handler = page_handler };
    httpd_uri_t update = { .uri = "/update", .method = HTTP_POST, .handler = update_handler };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &update);

    ESP_LOGI(TAG, "OTA window open for %d s. Join '%s', open http://192.168.4.1 (BOOT = skip)",
             OTA_WINDOW_MS / 1000, OTA_WIFI_SSID);

    int elapsed = 0;
    while (elapsed < OTA_WINDOW_MS || s_busy) {
        if (!s_busy && gpio_get_level(PIN_BOOT_BUTTON) == 0) {
            ESP_LOGI(TAG, "BOOT pressed, skipping OTA window");
            break;
        }
        gpio_set_level(PIN_USER_LED, (elapsed / 250) % 2);
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }

    httpd_stop(server);
#if !USE_TELEMETRY
    wifi_ap_off();           /* Only shut down Wi-Fi if telemetry is disabled! */
#endif
    gpio_set_level(PIN_USER_LED, 0);
}