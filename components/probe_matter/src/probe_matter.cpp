#include "probe_matter.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"

#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <led_strip.h>
#include <platform/ESP32/OpenthreadLauncher.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "probe_thread.h"

#ifndef CONFIG_PROBE_BOARD_LED_ADDRESSABLE
#define CONFIG_PROBE_BOARD_LED_ADDRESSABLE 1
#endif

#ifndef CONFIG_PROBE_BOARD_LED_GPIO
#define CONFIG_PROBE_BOARD_LED_GPIO 0
#endif

#ifndef CONFIG_PROBE_BOARD_LED_GPIO_NUM
#define CONFIG_PROBE_BOARD_LED_GPIO_NUM 8
#endif

#ifndef CONFIG_PROBE_BOARD_LED_BRIGHTNESS
#define CONFIG_PROBE_BOARD_LED_BRIGHTNESS 16
#endif

#ifndef CONFIG_PROBE_BOARD_LED_ACTIVE_HIGH
#define CONFIG_PROBE_BOARD_LED_ACTIVE_HIGH 1
#endif

using namespace esp_matter;
using namespace esp_matter::endpoint;

static const char *TAG = "probe_matter";
static uint16_t s_endpoint_id;
#if CONFIG_PROBE_BOARD_LED_ADDRESSABLE
static led_strip_handle_t s_led_strip;
#endif

static esp_err_t board_led_set(bool on)
{
#if CONFIG_PROBE_BOARD_LED_ADDRESSABLE
    if (!s_led_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 0, 0, CONFIG_PROBE_BOARD_LED_BRIGHTNESS, 0),
                            TAG, "set board RGB LED failed");
        return led_strip_refresh(s_led_strip);
    }
    return led_strip_clear(s_led_strip);
#elif CONFIG_PROBE_BOARD_LED_GPIO
    const int level = on ? CONFIG_PROBE_BOARD_LED_ACTIVE_HIGH : !CONFIG_PROBE_BOARD_LED_ACTIVE_HIGH;
    gpio_set_level((gpio_num_t)CONFIG_PROBE_BOARD_LED_GPIO_NUM, level);
    return ESP_OK;
#else
    (void)on;
    return ESP_OK;
#endif
}

static esp_err_t board_led_init(void)
{
#if CONFIG_PROBE_BOARD_LED_ADDRESSABLE
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_PROBE_BOARD_LED_GPIO_NUM,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip),
                        TAG, "board RGB LED init failed");
    return board_led_set(false);
#elif CONFIG_PROBE_BOARD_LED_GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_PROBE_BOARD_LED_GPIO_NUM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "board GPIO LED init failed");
    return board_led_set(false);
#else
    return ESP_OK;
#endif
}

static void matter_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Matter commissioning window opened");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Matter commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Matter commissioning failed: fail-safe timer expired");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Matter commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Matter fabric removed");
        break;
    default:
        break;
    }
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type,
                                     uint16_t endpoint_id,
                                     uint32_t cluster_id,
                                     uint32_t attribute_id,
                                     esp_matter_attr_val_t *val,
                                     void *priv_data)
{
    (void)priv_data;

    if (type == attribute::POST_UPDATE && endpoint_id == s_endpoint_id && cluster_id == 0x00000006 &&
        attribute_id == 0x00000000 && val && val->type == ESP_MATTER_VAL_TYPE_BOOLEAN) {
        esp_err_t err = board_led_set(val->val.b);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "board LED update failed: %s", esp_err_to_name(err));
        }
    }

    return ESP_OK;
}

static esp_err_t identification_cb(identification::callback_type_t type,
                                   uint16_t endpoint_id,
                                   uint8_t effect_id,
                                   uint8_t effect_variant,
                                   void *priv_data)
{
    (void)priv_data;
    ESP_LOGI(TAG, "identify endpoint=%u type=%u effect=%u variant=%u",
             endpoint_id, type, effect_id, effect_variant);
    return ESP_OK;
}

static void configure_thread_platform(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    static esp_openthread_platform_config_t config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size = 10,
        },
    };
    set_openthread_platform_config(&config);
#endif
}

static esp_err_t prepare_thread_identity_before_matter_start(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_err_t event_err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(event_err == ESP_OK || event_err == ESP_ERR_INVALID_STATE,
                        event_err,
                        TAG,
                        "default event loop init failed");

    ESP_RETURN_ON_ERROR(openthread_init_stack(), TAG, "openthread pre-init failed");

    otInstance *instance = esp_openthread_get_instance();
    ESP_RETURN_ON_FALSE(instance != nullptr, ESP_FAIL, TAG, "missing openthread instance");
    ESP_RETURN_ON_FALSE(esp_openthread_lock_acquire(portMAX_DELAY), ESP_FAIL, TAG, "openthread lock acquire failed");

    esp_err_t err = probe_thread_apply_deterministic_ext_addr(instance);
    if (err == ESP_OK) {
        err = probe_thread_register_state_logger(instance);
    }

    esp_openthread_lock_release();
    return err;
#else
    return ESP_OK;
#endif
}

esp_err_t probe_matter_start(void)
{
    ESP_RETURN_ON_ERROR(board_led_init(), TAG, "board LED init failed");

    node::config_t node_config;
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "failed to create Matter node");
        return ESP_FAIL;
    }

    on_off_light::config_t light_config;
    light_config.on_off.on_off = false;

    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        ESP_LOGE(TAG, "failed to create Matter endpoint");
        return ESP_FAIL;
    }
    s_endpoint_id = endpoint::get_id(endpoint);

    configure_thread_platform();
    ESP_RETURN_ON_ERROR(prepare_thread_identity_before_matter_start(), TAG, "Thread identity init failed");

    esp_err_t err = esp_matter::start(matter_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Matter: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Matter placeholder On/Off endpoint id=%u", s_endpoint_id);
    ESP_LOGI(TAG, "Default manual pairing code: 34970112332");
    ESP_LOGI(TAG, "Default QR payload: MT:Y.K9042C00KA0648G00");
    ESP_LOGI(TAG, "QR image URL: https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:Y.K9042C00KA0648G00");
    ESP_LOGI(TAG, "Scan this with Apple Home or Home Assistant; Thread credentials are sent over BLE.");

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::init();
#endif
    return ESP_OK;
}
