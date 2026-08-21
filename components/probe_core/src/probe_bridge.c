#include "probe_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "probe_json.h"
#include "probe_runtime.h"
#include "probe_thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "probe_bridge";

#ifndef CONFIG_PROBE_UPLINK_UART_NUM
#define CONFIG_PROBE_UPLINK_UART_NUM 1
#endif

#ifndef CONFIG_PROBE_UPLINK_UART_TX_GPIO
#define CONFIG_PROBE_UPLINK_UART_TX_GPIO 4
#endif

#ifndef CONFIG_PROBE_UPLINK_UART_RX_GPIO
#define CONFIG_PROBE_UPLINK_UART_RX_GPIO 5
#endif

#ifndef CONFIG_PROBE_UPLINK_UART_BAUD
#define CONFIG_PROBE_UPLINK_UART_BAUD 115200
#endif

static cJSON *health_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "board", "esp32c6-probe");
    cJSON_AddStringToObject(root, "phase", probe_runtime_phase_name(probe_runtime_get_phase()));
    cJSON_AddBoolToObject(root, "matter_started", probe_runtime_matter_started());
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return root;
}

static cJSON *info_json(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "esp-thread-probe-c6");
    cJSON_AddStringToObject(root, "version", "0.2.0-dual-mcu");
    cJSON_AddStringToObject(root, "transport", "uart-jsonl");
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "chip_model", chip.model);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddItemToObject(root, "thread", probe_thread_info_json());
    return root;
}

static cJSON *error_json(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "error", message);
    return root;
}

static cJSON *dispatch_command(const char *line)
{
    if (strcmp(line, "GET /health") == 0 || strcmp(line, "health") == 0) {
        return health_json();
    }
    if (strcmp(line, "GET /info") == 0 || strcmp(line, "info") == 0) {
        return info_json();
    }
    if (strcmp(line, "GET /mesh") == 0 || strcmp(line, "mesh") == 0) {
        return probe_thread_mesh_json();
    }
    if (strcmp(line, "GET /neighbors") == 0 || strcmp(line, "neighbors") == 0) {
        return probe_thread_neighbors_json();
    }
    if (strcmp(line, "GET /routers") == 0 || strcmp(line, "routers") == 0) {
        return probe_thread_routers_json();
    }
    if (strcmp(line, "GET /children") == 0 || strcmp(line, "children") == 0) {
        return probe_thread_children_json();
    }
    if (strcmp(line, "GET /topology") == 0 || strcmp(line, "topology") == 0) {
        return probe_thread_topology_json();
    }
    if (strcmp(line, "GET /router-neighbors") == 0 || strcmp(line, "router-neighbors") == 0) {
        return probe_thread_router_neighbors_json();
    }
    if (strcmp(line, "GET /router-neighbors/scan") == 0 || strcmp(line, "router-neighbors/scan") == 0) {
        return probe_thread_router_neighbors_scan_json();
    }
    if (strcmp(line, "GET /router") == 0 || strcmp(line, "router") == 0) {
        return probe_thread_router_json();
    }
    if (strcmp(line, "GET /ipaddr") == 0 || strcmp(line, "ipaddr") == 0) {
        return probe_thread_ipaddr_json();
    }
    if (strcmp(line, "GET /leader") == 0 || strcmp(line, "leader") == 0) {
        return probe_thread_leader_json();
    }
    if (strcmp(line, "GET /dataset") == 0 || strcmp(line, "dataset") == 0) {
        return probe_thread_dataset_json();
    }
    return error_json("unknown command");
}

static void send_json_line(cJSON *root)
{
    char *payload = probe_json_print_and_delete(root);
    if (!payload) {
        static const char oom[] = "{\"status\":\"error\",\"error\":\"json allocation failed\"}\n";
        uart_write_bytes(CONFIG_PROBE_UPLINK_UART_NUM, oom, sizeof(oom) - 1);
        return;
    }

    uart_write_bytes(CONFIG_PROBE_UPLINK_UART_NUM, payload, strlen(payload));
    uart_write_bytes(CONFIG_PROBE_UPLINK_UART_NUM, "\n", 1);
    free(payload);
}

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t pos = 0;
    bool discard_until_newline = false;

    ESP_LOGI(TAG, "uart bridge ready on UART%d tx=%d rx=%d baud=%d",
             CONFIG_PROBE_UPLINK_UART_NUM,
             CONFIG_PROBE_UPLINK_UART_TX_GPIO,
             CONFIG_PROBE_UPLINK_UART_RX_GPIO,
             CONFIG_PROBE_UPLINK_UART_BAUD);

    while (true) {
        uint8_t ch;
        int n = uart_read_bytes(CONFIG_PROBE_UPLINK_UART_NUM, &ch, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            continue;
        }

        if (discard_until_newline) {
            if (ch == '\n') {
                discard_until_newline = false;
                pos = 0;
            }
            continue;
        }

        if (ch == '\n') {
            line[pos] = '\0';
            trim_line(line);
            if (line[0]) {
                if (line[0] == '{' || line[0] == '[') {
                    ESP_LOGW(TAG, "discarding echoed JSON frame");
                } else {
                    ESP_LOGI(TAG, "uart request: %s", line);
                    send_json_line(dispatch_command(line));
                }
            }
            pos = 0;
        } else if (pos == 0 && (ch == '{' || ch == '[')) {
            discard_until_newline = true;
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;
            discard_until_newline = true;
            ESP_LOGW(TAG, "discarding overlong UART command");
        }
    }
}

esp_err_t probe_bridge_start(void)
{
#if CONFIG_PROBE_UPLINK_UART_ENABLE
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_PROBE_UPLINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_PROBE_UPLINK_UART_NUM, 4096, 4096, 0, NULL, 0),
                        TAG, "uart driver install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_PROBE_UPLINK_UART_NUM, &uart_config),
                        TAG, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_PROBE_UPLINK_UART_NUM,
                                     CONFIG_PROBE_UPLINK_UART_TX_GPIO,
                                     CONFIG_PROBE_UPLINK_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart pin config failed");

    xTaskCreate(bridge_task, "probe_uart_bridge", 6144, NULL, 6, NULL);
#endif
    return ESP_OK;
}
