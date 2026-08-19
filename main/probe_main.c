#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "probe_bridge.h"
#include "probe_matter.h"
#include "probe_runtime.h"
#include "probe_thread.h"

static const char *TAG = "thread_probe";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "starting esp-thread-probe");
    esp_log_level_set("OPENTHREAD", ESP_LOG_ERROR);
    ESP_ERROR_CHECK(probe_bridge_start());
    probe_runtime_set_phase(PROBE_RUNTIME_MATTER_STARTING);
    ESP_ERROR_CHECK(probe_matter_start());
    ESP_ERROR_CHECK(probe_thread_start_background_scan());
    probe_runtime_set_phase(PROBE_RUNTIME_MATTER_STARTED);
    ESP_LOGI(TAG, "Matter/OpenThread startup complete");
}
