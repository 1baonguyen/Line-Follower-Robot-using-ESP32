/* telemetry.c */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "telemetry.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define LINE_MAX_LEN   160      /* longer lines are cut */
#define QUEUE_LINES    32       /* lines waiting to be sent */
#define PACKET_MAX     1000     /* bytes per UDP packet */

typedef struct {
    uint16_t len;
    char     text[LINE_MAX_LEN];
} line_t;

static QueueHandle_t      s_queue = NULL;
static int                s_sock  = -1;
static struct sockaddr_in s_dest;
static uint32_t           s_dropped = 0;
static vprintf_like_t     s_uart_vprintf = NULL;

/* ---------------- log -> queue ---------------- */

static void enqueue(const char *text, int len)
{
    if (s_queue == NULL || len <= 0) {
        return;
    }

    line_t line;
    bool cut = (len >= LINE_MAX_LEN);
    if (cut) {
        len = LINE_MAX_LEN - 1;
    }
    memcpy(line.text, text, len);
    if (cut) {
        line.text[len - 1] = '\n';
    }
    line.len = (uint16_t)len;

    if (xQueueSend(s_queue, &line, 0) != pdTRUE) {
        __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
    }
}

static int log_hook(const char *fmt, va_list args)
{
    char buf[LINE_MAX_LEN];
    va_list copy;
    va_copy(copy, args);

    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    enqueue(buf, n);

    if (TELEMETRY_ALSO_UART && s_uart_vprintf != NULL) {
        s_uart_vprintf(fmt, copy);
    }
    va_end(copy);
    return n;
}

/* ---------------- queue -> UDP ---------------- */

static void sender_task(void *arg)
{
    static char packet[PACKET_MAX + LINE_MAX_LEN];
    line_t line;

    for (;;) {
        if (xQueueReceive(s_queue, &line, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int used = 0;
        uint32_t dropped = __atomic_exchange_n(&s_dropped, 0, __ATOMIC_RELAXED);
        if (dropped > 0) {
            used = snprintf(packet, 64, "[telemetry: %lu lines dropped]\n", (unsigned long)dropped);
        }

        do {
            memcpy(packet + used, line.text, line.len);
            used += line.len;
        } while (used < PACKET_MAX && xQueueReceive(s_queue, &line, 0) == pdTRUE);

        sendto(s_sock, packet, used, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
    }
}

/* ---------------- public ---------------- */

esp_err_t telemetry_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family      = AF_INET;
    s_dest.sin_port        = htons(TELEMETRY_PORT);
    s_dest.sin_addr.s_addr = inet_addr(TELEMETRY_DEST_IP);

    s_queue = xQueueCreate(QUEUE_LINES, sizeof(line_t));
    if (s_queue == NULL) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(sender_task, "telemetry", 4096, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    s_uart_vprintf = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

void telemetry_printf(const char *fmt, ...)
{
    char buf[LINE_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    enqueue(buf, n);
}