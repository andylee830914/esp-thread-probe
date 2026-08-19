#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "probe_uplink";
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_probe_lock;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = 1000;
static volatile uint8_t s_last_disconnect_reason;
static int64_t s_wifi_disconnected_since_us;
static int64_t s_last_wifi_driver_reset_us;
static const int WIFI_CONNECTED_BIT = BIT0;
static const size_t PROBE_RESPONSE_BUFFER_SIZE = 32768;

#define UPLINK_WIFI_WATCHDOG_PERIOD_MS 5000
#define UPLINK_WIFI_DRIVER_RESET_MS 60000
#define UPLINK_WIFI_RESTART_MS 300000

static void schedule_reconnect(uint8_t reason);
static const char *disconnect_reason_to_string(uint8_t reason);

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static const char *path_to_probe_command(const char *path)
{
    if (strcmp(path, "/health") == 0) {
        return "GET /health\n";
    }
    if (strcmp(path, "/info") == 0) {
        return "GET /info\n";
    }
    if (strcmp(path, "/mesh") == 0) {
        return "GET /mesh\n";
    }
    if (strcmp(path, "/neighbors") == 0) {
        return "GET /neighbors\n";
    }
    if (strcmp(path, "/routers") == 0) {
        return "GET /routers\n";
    }
    if (strcmp(path, "/children") == 0) {
        return "GET /children\n";
    }
    if (strcmp(path, "/topology") == 0) {
        return "GET /topology\n";
    }
    if (strcmp(path, "/router-neighbors") == 0) {
        return "GET /router-neighbors\n";
    }
    if (strcmp(path, "/router-neighbors/scan") == 0) {
        return "GET /router-neighbors/scan\n";
    }
    if (strcmp(path, "/router") == 0) {
        return "GET /router\n";
    }
    if (strcmp(path, "/ipaddr") == 0) {
        return "GET /ipaddr\n";
    }
    if (strcmp(path, "/leader") == 0) {
        return "GET /leader\n";
    }
    if (strcmp(path, "/dataset") == 0) {
        return "GET /dataset\n";
    }
    return NULL;
}

static esp_err_t probe_request(const char *command, char *response, size_t response_len)
{
    if (xSemaphoreTake(s_probe_lock, pdMS_TO_TICKS(CONFIG_UPLINK_PROBE_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uart_flush_input(CONFIG_UPLINK_UART_NUM);
    int written = uart_write_bytes(CONFIG_UPLINK_UART_NUM, command, strlen(command));
    ESP_LOGI(TAG, "uart tx %d bytes: %s", written, command);
    uart_wait_tx_done(CONFIG_UPLINK_UART_NUM, pdMS_TO_TICKS(100));

    size_t pos = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)CONFIG_UPLINK_PROBE_TIMEOUT_MS * 1000;

    bool overflow = false;
    bool complete = false;

    while (esp_timer_get_time() < deadline) {
        uint8_t ch;
        int n = uart_read_bytes(CONFIG_UPLINK_UART_NUM, &ch, 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        if (ch == '\n') {
            complete = true;
            break;
        }
        if (ch != '\r') {
            if (pos < response_len - 1) {
                response[pos++] = (char)ch;
            } else {
                overflow = true;
            }
        }
    }

    response[pos] = '\0';
    xSemaphoreGive(s_probe_lock);

    ESP_LOGI(TAG, "uart rx %u bytes%s", (unsigned)pos, overflow ? " overflow" : "");
    if (overflow) {
        return ESP_ERR_NO_MEM;
    }
    if (!complete) {
        return ESP_ERR_TIMEOUT;
    }
    return pos > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t send_json(httpd_req_t *req, const char *payload)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, payload);
}

static bool is_client_disconnect_errno(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EPIPE || err == ECONNABORTED ||
           err == ECONNRESET || err == ENOTCONN || err == EHOSTUNREACH;
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wifi reconnecting");
    esp_wifi_connect();
}

static void schedule_reconnect(uint8_t reason)
{
    if (!s_reconnect_timer) {
        return;
    }

    if (esp_timer_is_active(s_reconnect_timer)) {
        return;
    }

    if (reason == WIFI_REASON_NO_AP_FOUND) {
        s_reconnect_delay_ms = s_reconnect_delay_ms < 10000 ? s_reconnect_delay_ms * 2 : 10000;
    } else {
        s_reconnect_delay_ms = 1000;
    }

    ESP_LOGI(TAG, "wifi reconnect scheduled in %lu ms", (unsigned long)s_reconnect_delay_ms);
    esp_timer_start_once(s_reconnect_timer, s_reconnect_delay_ms * 1000ULL);
}

static void wifi_watchdog_task(void *arg)
{
    (void)arg;

    while (true) {
        wifi_ap_record_t ap = {0};
        bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        bool has_ip = (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;

        if (associated && has_ip) {
            ESP_LOGI(TAG, "wifi watchdog state=connected rssi=%d channel=%u", ap.rssi, ap.primary);
        } else {
            int64_t now = esp_timer_get_time();
            if (s_wifi_disconnected_since_us == 0) {
                s_wifi_disconnected_since_us = now;
            }

            int64_t disconnected_ms = (now - s_wifi_disconnected_since_us) / 1000;
            ESP_LOGW(TAG, "wifi watchdog state=%s disconnected_ms=%" PRId64 " last_reason=%u (%s)",
                     associated ? "associated_no_ip" : "disconnected",
                     disconnected_ms,
                     s_last_disconnect_reason,
                     disconnect_reason_to_string(s_last_disconnect_reason));

            if (s_reconnect_timer && !esp_timer_is_active(s_reconnect_timer)) {
                schedule_reconnect(s_last_disconnect_reason);
            }

            if (disconnected_ms >= UPLINK_WIFI_RESTART_MS) {
                ESP_LOGE(TAG, "wifi unavailable for %" PRId64 " ms, restarting board", disconnected_ms);
                esp_restart();
            }

            if (disconnected_ms >= UPLINK_WIFI_DRIVER_RESET_MS &&
                now - s_last_wifi_driver_reset_us >= (int64_t)UPLINK_WIFI_DRIVER_RESET_MS * 1000) {
                ESP_LOGW(TAG, "wifi unavailable for %" PRId64 " ms, restarting wifi driver", disconnected_ms);
                s_last_wifi_driver_reset_us = now;
                s_reconnect_delay_ms = 1000;
                if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
                    esp_timer_stop(s_reconnect_timer);
                }
                esp_wifi_disconnect();
                esp_wifi_stop();
                esp_wifi_start();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UPLINK_WIFI_WATCHDOG_PERIOD_MS));
    }
}

static const char *disconnect_reason_to_string(uint8_t reason)
{
    switch (reason) {
#ifdef WIFI_REASON_UNSPECIFIED
    case WIFI_REASON_UNSPECIFIED:
        return "unspecified";
#endif
#ifdef WIFI_REASON_AUTH_EXPIRE
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth_expire";
#endif
#ifdef WIFI_REASON_AUTH_LEAVE
    case WIFI_REASON_AUTH_LEAVE:
        return "auth_leave";
#endif
    case WIFI_REASON_NO_AP_FOUND:
        return "no_ap_found";
    case WIFI_REASON_AUTH_FAIL:
        return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc_fail";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
        return "class2_frame_from_nonauth_sta";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
        return "class3_frame_from_nonassoc_sta";
    case WIFI_REASON_ASSOC_LEAVE:
        return "assoc_leave";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "assoc_not_authed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon_timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake_timeout";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "no_compatible_security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "no_ap_in_auth_threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "no_ap_in_rssi_threshold";
    default:
        return "other";
    }
}

static esp_err_t proxy_get_handler(httpd_req_t *req)
{
    const char *command = path_to_probe_command(req->uri);
    if (!command) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown endpoint");
    }

    const size_t payload_size = PROBE_RESPONSE_BUFFER_SIZE;
    char *payload = malloc(payload_size);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    esp_err_t err = probe_request(command, payload, payload_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe request failed for %s: %s", req->uri, esp_err_to_name(err));
        const char *error = err == ESP_ERR_NO_MEM ? "probe response too large" : "probe timeout";
        snprintf(payload, payload_size,
                 "{\"status\":\"error\",\"source\":\"uplink\",\"error\":\"%s\",\"buffer_size\":%u,\"uptime_ms\":%lld}",
                 error, (unsigned)payload_size, esp_timer_get_time() / 1000);
    }

    ESP_LOGI(TAG, "http %s -> %u bytes", req->uri, (unsigned) strlen(payload));
    esp_err_t send_err = send_json(req, payload);
    if (send_err != ESP_OK) {
        int sock_errno = errno;
        if (is_client_disconnect_errno(sock_errno)) {
            ESP_LOGW(TAG, "client closed connection while sending %s (errno=%d)", req->uri, sock_errno);
            send_err = ESP_OK;
        } else {
            ESP_LOGW(TAG, "http send failed for %s: %s (errno=%d)", req->uri, esp_err_to_name(send_err), sock_errno);
        }
    }
    free(payload);
    return send_err;
}

static esp_err_t uplink_get_handler(httpd_req_t *req)
{
    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"ok\",\"source\":\"uplink\",\"name\":\"esp-thread-probe-uplink\",\"wifi_connected\":%s,"
             "\"uart_num\":%d,\"uart_tx_gpio\":%d,\"uart_rx_gpio\":%d,"
             "\"uart_baud\":%d,\"probe_timeout_ms\":%d,\"uptime_ms\":%lld}",
             json_bool((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0),
             CONFIG_UPLINK_UART_NUM,
             CONFIG_UPLINK_UART_TX_GPIO,
             CONFIG_UPLINK_UART_RX_GPIO,
             CONFIG_UPLINK_UART_BAUD,
             CONFIG_UPLINK_PROBE_TIMEOUT_MS,
             esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "http %s -> %u bytes", req->uri, (unsigned) strlen(payload));
    esp_err_t send_err = send_json(req, payload);
    if (send_err != ESP_OK) {
        int sock_errno = errno;
        if (is_client_disconnect_errno(sock_errno)) {
            ESP_LOGW(TAG, "client closed connection while sending %s (errno=%d)", req->uri, sock_errno);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "http send failed for %s: %s (errno=%d)", req->uri, esp_err_to_name(send_err), sock_errno);
    }
    return send_err;
}

static void register_get(httpd_handle_t server, const char *path)
{
    httpd_uri_t uri = {
        .uri = path,
        .method = HTTP_GET,
        .handler = proxy_get_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}

static esp_err_t http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_UPLINK_HTTP_PORT;
    config.max_uri_handlers = 16;
    config.stack_size = 6144;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 8;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "http server start failed");
    register_get(server, "/health");
    register_get(server, "/info");
    register_get(server, "/mesh");
    register_get(server, "/neighbors");
    register_get(server, "/routers");
    register_get(server, "/children");
    register_get(server, "/topology");
    register_get(server, "/router-neighbors");
    register_get(server, "/router-neighbors/scan");
    register_get(server, "/router");
    register_get(server, "/ipaddr");
    register_get(server, "/leader");
    register_get(server, "/dataset");
    httpd_uri_t uplink = {
        .uri = "/uplink",
        .method = HTTP_GET,
        .handler = uplink_get_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uplink));
    ESP_LOGI(TAG, "http api listening on port %d", CONFIG_UPLINK_HTTP_PORT);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "wifi connected ssid=%.*s channel=%u authmode=%u",
                 event ? event->ssid_len : 0,
                 event ? (const char *)event->ssid : "",
                 event ? event->channel : 0,
                 event ? event->authmode : 0);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event ? event->reason : 0;
        s_last_disconnect_reason = reason;
        if (s_wifi_disconnected_since_us == 0) {
            s_wifi_disconnected_since_us = esp_timer_get_time();
        }
        ESP_LOGW(TAG, "wifi disconnected, reason=%u (%s)", reason, disconnect_reason_to_string(reason));
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        schedule_reconnect(reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "wifi got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_reconnect_delay_ms = 1000;
        s_wifi_disconnected_since_us = 0;
        s_last_wifi_driver_reset_us = 0;
        if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
            esp_timer_stop(s_reconnect_timer);
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "wifi event group allocation failed");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_timer_cb,
        .name = "uplink_wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_UPLINK_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_UPLINK_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.failure_retry_cnt = 5;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(wifi_watchdog_task, "uplink_wifi_watchdog", 3072, NULL, 6, NULL);
    return ESP_OK;
}

static esp_err_t uart_start(void)
{
    s_probe_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_probe_lock, ESP_ERR_NO_MEM, TAG, "probe mutex allocation failed");

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_UPLINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_UPLINK_UART_NUM, 4096, 4096, 0, NULL, 0),
                        TAG, "uart driver install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_UPLINK_UART_NUM, &uart_config),
                        TAG, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_UPLINK_UART_NUM,
                                     CONFIG_UPLINK_UART_TX_GPIO,
                                     CONFIG_UPLINK_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart pin config failed");
    ESP_LOGI(TAG, "probe uart ready on UART%d tx=%d rx=%d baud=%d",
             CONFIG_UPLINK_UART_NUM, CONFIG_UPLINK_UART_TX_GPIO,
             CONFIG_UPLINK_UART_RX_GPIO, CONFIG_UPLINK_UART_BAUD);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "starting esp-thread-probe uplink");
    ESP_ERROR_CHECK(uart_start());
    ESP_ERROR_CHECK(wifi_start());
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    ESP_ERROR_CHECK(http_start());
}
