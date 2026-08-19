#include "probe_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/netdiag.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

static const char *TAG = "probe_thread";

#define PROBE_MAX_ROUTER_DIAG_ENTRIES 64
#define PROBE_MAX_ROUTER_LINKS 64
#define PROBE_MAX_ROUTER_CHILDREN 64
#define PROBE_INVALID_ROUTER_ID (OT_NETWORK_MAX_ROUTER_ID + 1)

typedef struct {
    uint8_t router_id;
    uint8_t link_quality_in;
    uint8_t link_quality_out;
    uint8_t route_cost;
    uint8_t next_hop;
    uint8_t next_hop_cost;
    bool has_link;
} probe_router_link_t;

typedef struct {
    uint16_t child_id;
    uint16_t timeout;
    uint8_t link_quality;
    otLinkModeConfig mode;
    bool ext_address_pending;
    bool ext_address_responded;
    bool has_ext_address;
    otError ext_address_error;
    otExtAddress ext_address;
} probe_router_child_t;

typedef struct {
    uint8_t router_id;
    uint16_t rloc16;
    uint32_t scan_id;
    bool has_ext_address;
    otExtAddress ext_address;
    uint32_t updated_ms;
    uint8_t link_quality_1;
    uint8_t link_quality_2;
    uint8_t link_quality_3;
    uint8_t leader_cost;
    uint8_t active_routers;
    uint8_t child_count;
    uint8_t stored_child_count;
    uint8_t link_count;
    bool valid;
    bool pending;
    bool responded;
    bool child_mac_refresh_needed;
    otError error;
    probe_router_link_t links[PROBE_MAX_ROUTER_LINKS];
    probe_router_child_t children[PROBE_MAX_ROUTER_CHILDREN];
} probe_router_diag_entry_t;

typedef struct {
    uint8_t router_id;
    uint16_t rloc16;
} probe_router_diag_context_t;

static portMUX_TYPE s_router_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static probe_router_diag_entry_t s_router_diag_entries[PROBE_MAX_ROUTER_DIAG_ENTRIES];
static probe_router_diag_context_t s_router_diag_contexts[PROBE_MAX_ROUTER_DIAG_ENTRIES];
static uint32_t s_router_diag_scan_started_ms;
static uint32_t s_router_diag_scan_id;
static uint8_t s_router_diag_pending_count;
static uint32_t s_router_diag_last_auto_scan_ms;
static bool s_router_diag_worker_started;

#ifndef CONFIG_PROBE_THREAD_API_LOCK_TIMEOUT_MS
#define CONFIG_PROBE_THREAD_API_LOCK_TIMEOUT_MS 0
#endif

#ifndef CONFIG_PROBE_ROUTER_SCAN_STALE_MS
#define CONFIG_PROBE_ROUTER_SCAN_STALE_MS 120000
#endif

#ifndef CONFIG_PROBE_ROUTER_AUTO_SCAN_INTERVAL_MS
#define CONFIG_PROBE_ROUTER_AUTO_SCAN_INTERVAL_MS 600000
#endif

#ifndef CONFIG_PROBE_ROUTER_SCAN_WORKER_PERIOD_MS
#define CONFIG_PROBE_ROUTER_SCAN_WORKER_PERIOD_MS 500
#endif

#ifndef CONFIG_PROBE_ROUTER_PROGRESS_LOG_INTERVAL_MS
#define CONFIG_PROBE_ROUTER_PROGRESS_LOG_INTERVAL_MS 2000
#endif

#ifndef CONFIG_PROBE_ROUTER_WORKER_LOCK_TIMEOUT_MS
#define CONFIG_PROBE_ROUTER_WORKER_LOCK_TIMEOUT_MS 200
#endif

static const uint8_t s_router_diag_tlv_types[] = {
    OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS,
    OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS,
    OT_NETWORK_DIAGNOSTIC_TLV_CONNECTIVITY,
    OT_NETWORK_DIAGNOSTIC_TLV_CHILD_TABLE,
    OT_NETWORK_DIAGNOSTIC_TLV_ROUTE,
    OT_NETWORK_DIAGNOSTIC_TLV_ENHANCED_ROUTE,
};

static bool try_acquire_thread_lock(void)
{
    return esp_openthread_lock_acquire(pdMS_TO_TICKS(CONFIG_PROBE_THREAD_API_LOCK_TIMEOUT_MS));
}

static void format_ext_addr(const otExtAddress *addr, char *buf, size_t len)
{
    snprintf(buf,
             len,
             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             addr->m8[0],
             addr->m8[1],
             addr->m8[2],
             addr->m8[3],
             addr->m8[4],
             addr->m8[5],
             addr->m8[6],
             addr->m8[7]);
}

static void derive_ext_addr_from_factory_mac(const uint8_t factory_mac[6], otExtAddress *ext_addr)
{
    ext_addr->m8[0] = (factory_mac[0] | 0x02) & 0xfe;
    ext_addr->m8[1] = factory_mac[1];
    ext_addr->m8[2] = factory_mac[2];
    ext_addr->m8[3] = 0xff;
    ext_addr->m8[4] = 0xfe;
    ext_addr->m8[5] = factory_mac[3];
    ext_addr->m8[6] = factory_mac[4];
    ext_addr->m8[7] = factory_mac[5];
}

static bool ext_addr_equal(const otExtAddress *a, const otExtAddress *b)
{
    return memcmp(a->m8, b->m8, sizeof(a->m8)) == 0;
}

esp_err_t probe_thread_apply_deterministic_ext_addr(otInstance *instance)
{
    ESP_RETURN_ON_FALSE(instance != NULL, ESP_ERR_INVALID_ARG, TAG, "missing openthread instance");

    uint8_t factory_mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(factory_mac, ESP_MAC_EFUSE_FACTORY), TAG, "read ESP factory MAC failed");

    otExtAddress ext_addr = {0};
    derive_ext_addr_from_factory_mac(factory_mac, &ext_addr);

    char ext_addr_str[24];
    format_ext_addr(&ext_addr, ext_addr_str, sizeof(ext_addr_str));
    ESP_LOGI(TAG, "ESP Factory MAC : " MACSTR, MAC2STR(factory_mac));
    ESP_LOGI(TAG, "Thread ExtAddr  : %s", ext_addr_str);
    ESP_LOGI(TAG, "ExtAddr source  : deterministic-from-factory-mac");

    const otExtAddress *current = otLinkGetExtendedAddress(instance);
    if (current && ext_addr_equal(current, &ext_addr)) {
        return ESP_OK;
    }

    otError err = otLinkSetExtendedAddress(instance, &ext_addr);
    ESP_RETURN_ON_FALSE(err == OT_ERROR_NONE,
                        ESP_FAIL,
                        TAG,
                        "set deterministic Thread ExtAddr failed: %s",
                        err == OT_ERROR_INVALID_STATE ? "thread-enabled" : "openthread-error");

    return ESP_OK;
}

static bool role_is_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

static void log_thread_attach_state(otInstance *instance)
{
    const otExtAddress *ext_addr = otLinkGetExtendedAddress(instance);
    char ext_addr_str[24];
    format_ext_addr(ext_addr, ext_addr_str, sizeof(ext_addr_str));

    ESP_LOGI(TAG, "Thread attached");
    ESP_LOGI(TAG, "RLOC16          : 0x%04x", otThreadGetRloc16(instance));
    ESP_LOGI(TAG, "Thread ExtAddr  : %s", ext_addr_str);
}

static bool s_thread_state_logger_registered;
static bool s_thread_was_attached;

static void thread_state_changed_cb(otChangedFlags flags, void *context)
{
    if ((flags & OT_CHANGED_THREAD_ROLE) == 0) {
        return;
    }

    otInstance *instance = (otInstance *)context;
    otDeviceRole role = otThreadGetDeviceRole(instance);
    bool attached = role_is_attached(role);

    if (attached && !s_thread_was_attached) {
        log_thread_attach_state(instance);
    }
    s_thread_was_attached = attached;
}

esp_err_t probe_thread_register_state_logger(otInstance *instance)
{
    ESP_RETURN_ON_FALSE(instance != NULL, ESP_ERR_INVALID_ARG, TAG, "missing openthread instance");
    if (s_thread_state_logger_registered) {
        return ESP_OK;
    }

    otError err = otSetStateChangedCallback(instance, thread_state_changed_cb, instance);
    ESP_RETURN_ON_FALSE(err == OT_ERROR_NONE || err == OT_ERROR_ALREADY,
                        ESP_FAIL,
                        TAG,
                        "register Thread state logger failed");
    s_thread_state_logger_registered = true;

    otDeviceRole role = otThreadGetDeviceRole(instance);
    s_thread_was_attached = role_is_attached(role);
    if (s_thread_was_attached) {
        log_thread_attach_state(instance);
    }

    return ESP_OK;
}

static const char *role_to_string(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED:
        return "disabled";
    case OT_DEVICE_ROLE_DETACHED:
        return "detached";
    case OT_DEVICE_ROLE_CHILD:
        return "child";
    case OT_DEVICE_ROLE_ROUTER:
        return "router";
    case OT_DEVICE_ROLE_LEADER:
        return "leader";
    default:
        return "unknown";
    }
}

static void add_rloc16(cJSON *obj, const char *name, uint16_t rloc16)
{
    char value[8];
    snprintf(value, sizeof(value), "0x%04x", rloc16);
    cJSON_AddStringToObject(obj, name, value);
}

static void add_ext_address(cJSON *obj, const char *name, const otExtAddress *addr)
{
    char value[17];
    for (size_t i = 0; i < sizeof(addr->m8); ++i) {
        snprintf(&value[i * 2], sizeof(value) - (i * 2), "%02x", addr->m8[i]);
    }
    cJSON_AddStringToObject(obj, name, value);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void router_rloc_ip6(otInstance *instance, uint16_t rloc16, otIp6Address *addr)
{
    memset(addr, 0, sizeof(*addr));
    const otMeshLocalPrefix *prefix = otThreadGetMeshLocalPrefix(instance);
    memcpy(&addr->mFields.m8[0], prefix->m8, OT_IP6_PREFIX_SIZE);
    addr->mFields.m8[8] = 0x00;
    addr->mFields.m8[9] = 0x00;
    addr->mFields.m8[10] = 0x00;
    addr->mFields.m8[11] = 0xff;
    addr->mFields.m8[12] = 0xfe;
    addr->mFields.m8[13] = 0x00;
    addr->mFields.m8[14] = (uint8_t)(rloc16 >> 8);
    addr->mFields.m8[15] = (uint8_t)rloc16;
}

static probe_router_diag_entry_t *diag_entry_by_router_id(uint8_t router_id)
{
    if (router_id >= PROBE_MAX_ROUTER_DIAG_ENTRIES) {
        return NULL;
    }
    return &s_router_diag_entries[router_id];
}

static const char *ot_error_name(otError error)
{
    switch (error) {
    case OT_ERROR_NONE:
        return "ok";
    case OT_ERROR_RESPONSE_TIMEOUT:
        return "timeout";
    case OT_ERROR_NO_BUFS:
        return "no_bufs";
    default:
        return "error";
    }
}

static uint16_t child_rloc16_for_entry(const probe_router_diag_entry_t *entry, const probe_router_child_t *child)
{
    return (entry->rloc16 & 0xfc00) | (child->child_id & 0x01ff);
}

static probe_router_child_t *find_child_by_rloc16(probe_router_diag_entry_t *entry, uint16_t child_rloc16)
{
    for (uint8_t i = 0; i < entry->stored_child_count; ++i) {
        probe_router_child_t *child = &entry->children[i];
        if (child_rloc16_for_entry(entry, child) == child_rloc16) {
            return child;
        }
    }
    return NULL;
}

static void find_child_owner_by_rloc16(uint16_t child_rloc16,
                                       probe_router_diag_entry_t **owner_entry,
                                       probe_router_child_t **owner_child)
{
    *owner_entry = NULL;
    *owner_child = NULL;

    for (uint8_t router_id = 0; router_id < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++router_id) {
        probe_router_diag_entry_t *entry = &s_router_diag_entries[router_id];
        if (!entry->valid) {
            continue;
        }
        probe_router_child_t *child = find_child_by_rloc16(entry, child_rloc16);
        if (child) {
            *owner_entry = entry;
            *owner_child = child;
            return;
        }
    }
}

static bool child_mode_equal(const otLinkModeConfig *a, const otLinkModeConfig *b)
{
    return a->mRxOnWhenIdle == b->mRxOnWhenIdle &&
           a->mDeviceType == b->mDeviceType &&
           a->mNetworkData == b->mNetworkData;
}

static bool child_row_equal(const probe_router_child_t *a, const probe_router_child_t *b)
{
    return a->child_id == b->child_id &&
           a->timeout == b->timeout &&
           a->link_quality == b->link_quality &&
           child_mode_equal(&a->mode, &b->mode);
}

static probe_router_child_t *find_child_by_id(probe_router_diag_entry_t *entry, uint16_t child_id)
{
    for (uint8_t i = 0; i < entry->stored_child_count; ++i) {
        if (entry->children[i].child_id == child_id) {
            return &entry->children[i];
        }
    }
    return NULL;
}

static bool child_table_changed(const probe_router_diag_entry_t *previous, const probe_router_diag_entry_t *current)
{
    if (previous->stored_child_count != current->stored_child_count) {
        return true;
    }

    for (uint8_t i = 0; i < current->stored_child_count; ++i) {
        const probe_router_child_t *current_child = &current->children[i];
        probe_router_diag_entry_t prev_copy = *previous;
        probe_router_child_t *previous_child = find_child_by_id(&prev_copy, current_child->child_id);
        if (!previous_child || !child_row_equal(previous_child, current_child)) {
            return true;
        }
    }

    return false;
}

static void copy_child_mac_cache(const probe_router_diag_entry_t *previous, probe_router_diag_entry_t *current)
{
    probe_router_diag_entry_t prev_copy = *previous;
    for (uint8_t i = 0; i < current->stored_child_count; ++i) {
        probe_router_child_t *child = &current->children[i];
        probe_router_child_t *cached = find_child_by_id(&prev_copy, child->child_id);
        if (!cached || !cached->has_ext_address) {
            continue;
        }
        child->has_ext_address = true;
        child->ext_address = cached->ext_address;
        child->ext_address_responded = true;
        child->ext_address_error = OT_ERROR_NONE;
    }
}

static bool child_mac_refresh_done(const probe_router_diag_entry_t *entry)
{
    for (uint8_t i = 0; i < entry->stored_child_count; ++i) {
        const probe_router_child_t *child = &entry->children[i];
        if (!child->has_ext_address && !child->ext_address_responded && child->ext_address_error == OT_ERROR_NONE) {
            return false;
        }
        if (child->ext_address_pending) {
            return false;
        }
    }
    return true;
}

static void child_diag_callback(otError error, otMessage *message, const otMessageInfo *message_info, void *context)
{
    (void)context;
    uint16_t child_rloc16 = 0xfffe;
    otExtAddress ext_address = {0};
    bool has_ext_address = false;

    if (message_info) {
        child_rloc16 = ((uint16_t)message_info->mPeerAddr.mFields.m8[14] << 8) |
                       (uint16_t)message_info->mPeerAddr.mFields.m8[15];
    }

    if (error == OT_ERROR_NONE && message) {
        otNetworkDiagIterator iterator = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;
        otNetworkDiagTlv tlv = {0};

        while (otThreadGetNextDiagnosticTlv(message, &iterator, &tlv) == OT_ERROR_NONE) {
            if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS) {
                child_rloc16 = tlv.mData.mAddr16;
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS) {
                ext_address = tlv.mData.mExtAddress;
                has_ext_address = true;
            }
        }
    }

    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = NULL;
    probe_router_child_t *child = NULL;
    find_child_owner_by_rloc16(child_rloc16, &entry, &child);
    if (child && entry) {
        child->ext_address_pending = false;
        child->ext_address_responded = error == OT_ERROR_NONE;
        child->ext_address_error = error;
        child->has_ext_address = has_ext_address;
        if (has_ext_address) {
            child->ext_address = ext_address;
        }
        if (child_mac_refresh_done(entry)) {
            entry->child_mac_refresh_needed = false;
        }
    }
    if (s_router_diag_pending_count > 0) {
        s_router_diag_pending_count--;
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
}

static void request_next_child_ext_address(otInstance *instance)
{
    static const uint8_t child_tlv_types[] = {
        OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS,
        OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS,
    };

    uint8_t target_router_id = 0xff;
    uint8_t target_child_index = 0xff;
    uint16_t target_child_rloc16 = 0xfffe;

    taskENTER_CRITICAL(&s_router_diag_lock);
    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
        const probe_router_diag_entry_t *entry = &s_router_diag_entries[i];
        if (entry->scan_id != s_router_diag_scan_id) {
            continue;
        }
        if (entry->pending) {
            taskEXIT_CRITICAL(&s_router_diag_lock);
            return;
        }
        for (uint8_t j = 0; j < entry->stored_child_count; ++j) {
            if (entry->children[j].ext_address_pending) {
                taskEXIT_CRITICAL(&s_router_diag_lock);
                return;
            }
        }
    }

    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES && target_router_id == 0xff; ++i) {
        probe_router_diag_entry_t *entry = &s_router_diag_entries[i];
        if (entry->scan_id != s_router_diag_scan_id ||
            !entry->valid ||
            entry->pending ||
            !entry->responded ||
            entry->error != OT_ERROR_NONE ||
            !entry->child_mac_refresh_needed) {
            continue;
        }
        for (uint8_t j = 0; j < entry->stored_child_count; ++j) {
            const probe_router_child_t *child = &entry->children[j];
            if (child->has_ext_address || child->ext_address_pending || child->ext_address_responded) {
                continue;
            }
            target_router_id = i;
            target_child_index = j;
            target_child_rloc16 = child_rloc16_for_entry(entry, child);
            break;
        }
        if (child_mac_refresh_done(entry)) {
            entry->child_mac_refresh_needed = false;
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    if (target_router_id == 0xff) {
        return;
    }

    otIp6Address destination = {0};
    router_rloc_ip6(instance, target_child_rloc16, &destination);
    otError err = otThreadSendDiagnosticGet(instance,
                                            &destination,
                                            child_tlv_types,
                                            sizeof(child_tlv_types),
                                            child_diag_callback,
                                            NULL);

    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(target_router_id);
    if (entry && target_child_index < entry->stored_child_count) {
        probe_router_child_t *child = &entry->children[target_child_index];
        if (err == OT_ERROR_NONE) {
            child->ext_address_pending = true;
            child->ext_address_responded = false;
            child->ext_address_error = OT_ERROR_NONE;
            s_router_diag_pending_count++;
        } else {
            child->ext_address_pending = false;
            child->ext_address_responded = false;
            child->ext_address_error = err;
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
}

static probe_router_link_t *find_or_add_router_link(probe_router_diag_entry_t *entry, uint8_t router_id)
{
    for (uint8_t i = 0; i < entry->link_count; ++i) {
        if (entry->links[i].router_id == router_id) {
            return &entry->links[i];
        }
    }

    if (entry->link_count >= PROBE_MAX_ROUTER_LINKS) {
        return NULL;
    }

    probe_router_link_t *link = &entry->links[entry->link_count++];
    memset(link, 0, sizeof(*link));
    link->router_id = router_id;
    link->next_hop = PROBE_INVALID_ROUTER_ID;
    return link;
}

static void router_diag_callback(otError error, otMessage *message, const otMessageInfo *message_info, void *context)
{
    (void)message_info;
    probe_router_diag_context_t *query = (probe_router_diag_context_t *)context;
    probe_router_diag_entry_t *parsed = calloc(1, sizeof(*parsed));

    if (!parsed) {
        taskENTER_CRITICAL(&s_router_diag_lock);
        probe_router_diag_entry_t *entry = diag_entry_by_router_id(query ? query->router_id : 0xff);
        if (entry) {
            entry->pending = false;
            entry->responded = false;
            entry->error = OT_ERROR_NO_BUFS;
            entry->updated_ms = now_ms();
            entry->scan_id = s_router_diag_scan_id;
        }
        if (s_router_diag_pending_count > 0) {
            s_router_diag_pending_count--;
        }
        taskEXIT_CRITICAL(&s_router_diag_lock);
        return;
    }

    parsed->router_id = query ? query->router_id : 0xff;
    parsed->rloc16 = query ? query->rloc16 : 0xfffe;
    parsed->updated_ms = now_ms();
    parsed->responded = error == OT_ERROR_NONE;
    parsed->error = error;

    if (error == OT_ERROR_NONE && message) {
        otNetworkDiagIterator iterator = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;
        otNetworkDiagTlv tlv = {0};

        while (otThreadGetNextDiagnosticTlv(message, &iterator, &tlv) == OT_ERROR_NONE) {
            if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS) {
                parsed->rloc16 = tlv.mData.mAddr16;
                parsed->router_id = (uint8_t)(parsed->rloc16 >> 10);
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS) {
                parsed->ext_address = tlv.mData.mExtAddress;
                parsed->has_ext_address = true;
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_CONNECTIVITY) {
                parsed->link_quality_1 = tlv.mData.mConnectivity.mLinkQuality1;
                parsed->link_quality_2 = tlv.mData.mConnectivity.mLinkQuality2;
                parsed->link_quality_3 = tlv.mData.mConnectivity.mLinkQuality3;
                parsed->leader_cost = tlv.mData.mConnectivity.mLeaderCost;
                parsed->active_routers = tlv.mData.mConnectivity.mActiveRouters;
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_CHILD_TABLE) {
                parsed->child_count = tlv.mData.mChildTable.mCount;
                parsed->stored_child_count = 0;
                for (uint8_t i = 0;
                     i < tlv.mData.mChildTable.mCount && parsed->stored_child_count < PROBE_MAX_ROUTER_CHILDREN;
                     ++i) {
                    const otNetworkDiagChildEntry *child = &tlv.mData.mChildTable.mTable[i];
                    probe_router_child_t *stored = &parsed->children[parsed->stored_child_count++];
                    stored->child_id = child->mChildId;
                    stored->timeout = child->mTimeout;
                    stored->link_quality = child->mLinkQuality;
                    stored->mode = child->mMode;
                    stored->ext_address_pending = false;
                    stored->ext_address_responded = false;
                    stored->has_ext_address = false;
                    stored->ext_address_error = OT_ERROR_NONE;
                }
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_ROUTE) {
                for (uint8_t i = 0; i < tlv.mData.mRoute.mRouteCount; ++i) {
                    const otNetworkDiagRouteData *route = &tlv.mData.mRoute.mRouteData[i];
                    if (route->mRouterId == parsed->router_id ||
                        (route->mLinkQualityIn == 0 && route->mLinkQualityOut == 0)) {
                        continue;
                    }
                    probe_router_link_t *link = find_or_add_router_link(parsed, route->mRouterId);
                    if (!link) {
                        continue;
                    }
                    link->has_link = true;
                    link->link_quality_in = route->mLinkQualityIn;
                    link->link_quality_out = route->mLinkQualityOut;
                    link->route_cost = route->mRouteCost;
                }
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_ENHANCED_ROUTE) {
                for (uint8_t i = 0; i < tlv.mData.mEnhRoute.mRouteCount && parsed->link_count < PROBE_MAX_ROUTER_LINKS; ++i) {
                    const otNetworkDiagEnhRouteData *route = &tlv.mData.mEnhRoute.mRouteData[i];
                    if (route->mIsSelf || !route->mHasLink) {
                        continue;
                    }
                    probe_router_link_t *link = find_or_add_router_link(parsed, route->mRouterId);
                    if (!link) {
                        continue;
                    }
                    link->has_link = route->mHasLink;
                    link->link_quality_in = route->mLinkQualityIn;
                    link->link_quality_out = route->mLinkQualityOut;
                    link->next_hop = route->mNextHop;
                    link->next_hop_cost = route->mNextHopCost;
                }
            }
        }

    }

    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(parsed->router_id);
    if (!entry && query) {
        entry = diag_entry_by_router_id(query->router_id);
    }
    if (entry) {
        if (error != OT_ERROR_NONE && entry->valid) {
            *parsed = *entry;
            parsed->responded = false;
            parsed->error = error;
            parsed->pending = false;
            parsed->updated_ms = now_ms();
        } else {
            bool changed = true;
            if (entry->valid && entry->responded && entry->error == OT_ERROR_NONE) {
                changed = child_table_changed(entry, parsed);
                copy_child_mac_cache(entry, parsed);
            }
            parsed->valid = true;
            parsed->pending = false;
            parsed->child_mac_refresh_needed = changed && parsed->stored_child_count > 0;
            if (parsed->child_mac_refresh_needed && child_mac_refresh_done(parsed)) {
                parsed->child_mac_refresh_needed = false;
            }
        }
        parsed->scan_id = s_router_diag_scan_id;
        *entry = *parsed;
    }
    if (s_router_diag_pending_count > 0) {
        s_router_diag_pending_count--;
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    free(parsed);
}

static void add_router_diag_entry_json(cJSON *routers, const probe_router_diag_entry_t *entry)
{
    cJSON *router = cJSON_CreateObject();
    cJSON_AddNumberToObject(router, "router_id", entry->router_id);
    add_rloc16(router, "rloc16", entry->rloc16);
    cJSON_AddStringToObject(router, "status", entry->pending ? "pending" : ot_error_name(entry->error));
    cJSON_AddBoolToObject(router, "responded", entry->responded);
    if (entry->has_ext_address) {
        add_ext_address(router, "ext_address", &entry->ext_address);
    } else {
        cJSON_AddNullToObject(router, "ext_address");
    }
    cJSON_AddNumberToObject(router, "updated_ms", entry->updated_ms);
    cJSON_AddNumberToObject(router, "leader_cost", entry->leader_cost);
    cJSON_AddNumberToObject(router, "active_routers", entry->active_routers);
    cJSON_AddNumberToObject(router, "child_count", entry->child_count);
    cJSON_AddNumberToObject(router, "stored_child_count", entry->stored_child_count);
    cJSON *lq = cJSON_AddObjectToObject(router, "link_quality_counts");
    cJSON_AddNumberToObject(lq, "lq1", entry->link_quality_1);
    cJSON_AddNumberToObject(lq, "lq2", entry->link_quality_2);
    cJSON_AddNumberToObject(lq, "lq3", entry->link_quality_3);
    cJSON *links = cJSON_AddArrayToObject(router, "router_neighbors");
    for (uint8_t j = 0; j < entry->link_count; ++j) {
        const probe_router_link_t *link = &entry->links[j];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "router_id", link->router_id);
        add_rloc16(item, "rloc16", (uint16_t)link->router_id << 10);
        cJSON_AddNumberToObject(item, "link_quality_in", link->link_quality_in);
        cJSON_AddNumberToObject(item, "link_quality_out", link->link_quality_out);
        cJSON_AddNumberToObject(item, "route_cost", link->route_cost);
        if (link->next_hop <= OT_NETWORK_MAX_ROUTER_ID) {
            cJSON_AddNumberToObject(item, "next_hop_router_id", link->next_hop);
            cJSON_AddNumberToObject(item, "next_hop_cost", link->next_hop_cost);
        } else {
            cJSON_AddNullToObject(item, "next_hop_router_id");
            cJSON_AddNullToObject(item, "next_hop_cost");
        }
        cJSON_AddItemToArray(links, item);
    }
    cJSON *children = cJSON_AddArrayToObject(router, "children");
    for (uint8_t j = 0; j < entry->stored_child_count; ++j) {
        const probe_router_child_t *child = &entry->children[j];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "child_id", child->child_id);
        add_rloc16(item, "rloc16", child_rloc16_for_entry(entry, child));
        cJSON_AddNumberToObject(item, "timeout", child->timeout);
        cJSON_AddNumberToObject(item, "link_quality", child->link_quality);
        if (child->has_ext_address) {
            add_ext_address(item, "ext_address", &child->ext_address);
            cJSON_AddStringToObject(item, "ext_address_status", "ok");
        } else {
            cJSON_AddNullToObject(item, "ext_address");
            if (child->ext_address_pending) {
                cJSON_AddStringToObject(item, "ext_address_status", "pending");
            } else if (child->ext_address_error != OT_ERROR_NONE) {
                cJSON_AddStringToObject(item, "ext_address_status", ot_error_name(child->ext_address_error));
            } else if (child->ext_address_responded) {
                cJSON_AddStringToObject(item, "ext_address_status", "no_ext_address");
            } else {
                cJSON_AddStringToObject(item, "ext_address_status", "not_requested");
            }
        }
        cJSON *mode = cJSON_AddObjectToObject(item, "mode");
        cJSON_AddBoolToObject(mode, "rx_on_when_idle", child->mode.mRxOnWhenIdle);
        cJSON_AddBoolToObject(mode, "full_thread_device", child->mode.mDeviceType);
        cJSON_AddBoolToObject(mode, "full_network_data", child->mode.mNetworkData);
        cJSON_AddItemToArray(children, item);
    }
    cJSON_AddItemToArray(routers, router);
}

static void add_router_diag_snapshot_json(cJSON *root)
{
    uint32_t scan_id;
    uint32_t scan_started_ms;
    uint32_t scan_age_ms;
    uint8_t pending_router = 0;
    uint16_t pending_child_mac = 0;

    taskENTER_CRITICAL(&s_router_diag_lock);
    scan_id = s_router_diag_scan_id;
    scan_started_ms = s_router_diag_scan_started_ms;
    taskEXIT_CRITICAL(&s_router_diag_lock);
    scan_age_ms = scan_started_ms > 0 ? (uint32_t)(now_ms() - scan_started_ms) : 0;

    cJSON_AddNumberToObject(root, "scan_id", scan_id);
    cJSON_AddNumberToObject(root, "scan_started_ms", scan_started_ms);
    cJSON_AddNumberToObject(root, "scan_age_ms", scan_age_ms);
    cJSON *routers = cJSON_AddArrayToObject(root, "routers");

    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
        probe_router_diag_entry_t entry;

        taskENTER_CRITICAL(&s_router_diag_lock);
        entry = s_router_diag_entries[i];
        taskEXIT_CRITICAL(&s_router_diag_lock);

        if (entry.scan_id != scan_id) {
            continue;
        }
        if (!entry.valid && !entry.pending) {
            continue;
        }
        if (entry.pending) {
            pending_router++;
        }
        for (uint8_t j = 0; j < entry.stored_child_count; ++j) {
            if (entry.children[j].ext_address_pending) {
                pending_child_mac++;
            }
        }
        add_router_diag_entry_json(routers, &entry);
    }

    cJSON_AddNumberToObject(root, "pending", pending_router);
    cJSON_AddNumberToObject(root, "pending_router", pending_router);
    cJSON_AddNumberToObject(root, "pending_child_mac", pending_child_mac);
    cJSON_AddNumberToObject(root, "pending_total", (uint32_t)pending_router + pending_child_mac);
}

static bool router_diag_scan_in_progress(uint32_t current_ms, bool *scan_stale)
{
    bool in_progress = false;
    bool stale = false;
    uint32_t scan_started_ms = 0;

    taskENTER_CRITICAL(&s_router_diag_lock);
    scan_started_ms = s_router_diag_scan_started_ms;
    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
        if (s_router_diag_entries[i].scan_id == s_router_diag_scan_id && s_router_diag_entries[i].pending) {
            in_progress = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    if (in_progress && scan_started_ms > 0) {
        stale = (uint32_t)(current_ms - scan_started_ms) > CONFIG_PROBE_ROUTER_SCAN_STALE_MS;
    }

    if (scan_stale) {
        *scan_stale = stale;
    }
    return in_progress;
}

static void log_router_diag_progress(void)
{
    static uint32_t s_last_log_scan_id = UINT32_MAX;
    static uint8_t s_last_pending_router = 0xff;
    static uint16_t s_last_pending_child = 0xffff;
    static uint8_t s_last_done_router = 0xff;
    static uint8_t s_last_total_router = 0xff;
    static uint32_t s_last_log_ms = 0;

    uint32_t scan_id = 0;
    uint8_t pending_router = 0;
    uint16_t pending_child_mac = 0;
    uint8_t done_router = 0;
    uint8_t total_router = 0;

    taskENTER_CRITICAL(&s_router_diag_lock);
    scan_id = s_router_diag_scan_id;
    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
        const probe_router_diag_entry_t *entry = &s_router_diag_entries[i];
        if (entry->scan_id != scan_id || !entry->valid) {
            continue;
        }
        total_router++;
        if (entry->pending) {
            pending_router++;
        } else {
            done_router++;
        }
        for (uint8_t j = 0; j < entry->stored_child_count; ++j) {
            if (entry->children[j].ext_address_pending) {
                pending_child_mac++;
            }
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    const uint32_t now = now_ms();
    const bool changed = scan_id != s_last_log_scan_id ||
                         pending_router != s_last_pending_router ||
                         pending_child_mac != s_last_pending_child ||
                         done_router != s_last_done_router ||
                         total_router != s_last_total_router;

    if (changed || (now - s_last_log_ms) >= CONFIG_PROBE_ROUTER_PROGRESS_LOG_INTERVAL_MS) {
        ESP_LOGI(TAG,
                 "router scan progress scan_id=%lu routers=%u/%u pending_router=%u pending_child_mac=%u",
                 (unsigned long)scan_id,
                 done_router,
                 total_router,
                 pending_router,
                 (unsigned)pending_child_mac);
        s_last_log_scan_id = scan_id;
        s_last_pending_router = pending_router;
        s_last_pending_child = pending_child_mac;
        s_last_done_router = done_router;
        s_last_total_router = total_router;
        s_last_log_ms = now;
    }
}

static bool start_router_diag_scan_with_lock(otInstance *instance, bool *scan_restarted_stale, uint8_t *sent)
{
    bool scan_stale = false;
    bool scan_in_progress = router_diag_scan_in_progress(now_ms(), &scan_stale);

    if (scan_restarted_stale) {
        *scan_restarted_stale = scan_in_progress && scan_stale;
    }
    if (sent) {
        *sent = 0;
    }

    if (scan_in_progress && !scan_stale) {
        return false;
    }

    uint8_t sent_local = 0;
    taskENTER_CRITICAL(&s_router_diag_lock);
    memset(s_router_diag_contexts, 0, sizeof(s_router_diag_contexts));
    s_router_diag_scan_id++;
    s_router_diag_scan_started_ms = now_ms();
    s_router_diag_pending_count = 0;
    taskEXIT_CRITICAL(&s_router_diag_lock);

    uint8_t max_router_id = otThreadGetMaxRouterId(instance);
    for (uint8_t router_id = 0; router_id <= max_router_id && router_id < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++router_id) {
        otRouterInfo info = {0};
        if (otThreadGetRouterInfo(instance, router_id, &info) != OT_ERROR_NONE || !info.mAllocated) {
            continue;
        }

        otIp6Address destination = {0};
        router_rloc_ip6(instance, info.mRloc16, &destination);
        s_router_diag_contexts[router_id].router_id = router_id;
        s_router_diag_contexts[router_id].rloc16 = info.mRloc16;

        taskENTER_CRITICAL(&s_router_diag_lock);
        probe_router_diag_entry_t *entry = diag_entry_by_router_id(router_id);
        if (entry) {
            entry->valid = true;
            entry->pending = true;
            entry->responded = false;
            entry->error = OT_ERROR_NONE;
            entry->router_id = router_id;
            entry->rloc16 = info.mRloc16;
            entry->updated_ms = now_ms();
            entry->scan_id = s_router_diag_scan_id;
            s_router_diag_pending_count++;
        }
        taskEXIT_CRITICAL(&s_router_diag_lock);

        otError err = otThreadSendDiagnosticGet(instance,
                                                &destination,
                                                s_router_diag_tlv_types,
                                                sizeof(s_router_diag_tlv_types),
                                                router_diag_callback,
                                                &s_router_diag_contexts[router_id]);
        if (err == OT_ERROR_NONE) {
            sent_local++;
        } else {
            taskENTER_CRITICAL(&s_router_diag_lock);
            entry = diag_entry_by_router_id(router_id);
            if (entry) {
                entry->pending = false;
                entry->responded = false;
                entry->error = err;
            }
            if (s_router_diag_pending_count > 0) {
                s_router_diag_pending_count--;
            }
            taskEXIT_CRITICAL(&s_router_diag_lock);
        }
    }

    if (sent) {
        *sent = sent_local;
    }
    return true;
}

static void add_router_json_locked(cJSON *root, otInstance *instance)
{
    otDeviceRole role = otThreadGetDeviceRole(instance);
    cJSON_AddStringToObject(root, "state", role_to_string(role));
    add_rloc16(root, "rloc16", otThreadGetRloc16(instance));

    otRouterInfo router_info = {0};
    if (otThreadGetRouterInfo(instance, otThreadGetRloc16(instance) >> 10, &router_info) == OT_ERROR_NONE) {
        cJSON_AddNumberToObject(root, "router_id", router_info.mRouterId);
        cJSON_AddNumberToObject(root, "link_quality_in", router_info.mLinkQualityIn);
        cJSON_AddNumberToObject(root, "link_quality_out", router_info.mLinkQualityOut);
    }

    otRouterInfo parent_info = {0};
    if (otThreadGetParentInfo(instance, &parent_info) == OT_ERROR_NONE) {
        cJSON *parent = cJSON_AddObjectToObject(root, "parent");
        add_rloc16(parent, "rloc16", parent_info.mRloc16);
        cJSON_AddNumberToObject(parent, "router_id", parent_info.mRouterId);
        cJSON_AddNumberToObject(parent, "link_quality_in", parent_info.mLinkQualityIn);
        cJSON_AddNumberToObject(parent, "link_quality_out", parent_info.mLinkQualityOut);
    } else {
        cJSON_AddNullToObject(root, "parent");
    }
}

static void add_ipaddr_json_locked(cJSON *root, otInstance *instance)
{
    cJSON *items = cJSON_AddArrayToObject(root, "addresses");
    char addr[OT_IP6_ADDRESS_STRING_SIZE];

    for (const otNetifAddress *a = otIp6GetUnicastAddresses(instance); a; a = a->mNext) {
        otIp6AddressToString(&a->mAddress, addr, sizeof(addr));
        cJSON_AddItemToArray(items, cJSON_CreateString(addr));
    }
}

static void add_leader_json_locked(cJSON *root, otInstance *instance)
{
    otLeaderData leader = {0};

    if (otThreadGetLeaderData(instance, &leader) == OT_ERROR_NONE) {
        cJSON_AddNumberToObject(root, "partition_id", leader.mPartitionId);
        cJSON_AddNumberToObject(root, "weighting", leader.mWeighting);
        cJSON_AddNumberToObject(root, "data_version", leader.mDataVersion);
        cJSON_AddNumberToObject(root, "stable_data_version", leader.mStableDataVersion);
        cJSON_AddNumberToObject(root, "router_id", leader.mLeaderRouterId);
    }
}

static void add_neighbors_json_locked(cJSON *root, otInstance *instance)
{
    cJSON *items = cJSON_AddArrayToObject(root, "neighbors");
    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo info = {0};

    while (otThreadGetNextNeighborInfo(instance, &iterator, &info) == OT_ERROR_NONE) {
        cJSON *item = cJSON_CreateObject();
        add_rloc16(item, "rloc16", info.mRloc16);
        cJSON_AddBoolToObject(item, "child", info.mIsChild);
        cJSON_AddNumberToObject(item, "age", info.mAge);
        cJSON_AddNumberToObject(item, "rssi", info.mAverageRssi);
        cJSON_AddNumberToObject(item, "link_quality_in", info.mLinkQualityIn);
        cJSON_AddNullToObject(item, "link_quality_out");
        cJSON_AddItemToArray(items, item);
    }
}

static void add_routers_json_locked(cJSON *root, otInstance *instance)
{
    cJSON *items = cJSON_AddArrayToObject(root, "routers");
    uint8_t max_router_id = otThreadGetMaxRouterId(instance);

    for (uint8_t router_id = 0; router_id <= max_router_id; ++router_id) {
        otRouterInfo info = {0};
        if (otThreadGetRouterInfo(instance, router_id, &info) != OT_ERROR_NONE || !info.mAllocated) {
            continue;
        }

        uint16_t next_hop_rloc16 = 0xfffe;
        uint8_t path_cost = 0;
        otThreadGetNextHopAndPathCost(instance, info.mRloc16, &next_hop_rloc16, &path_cost);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "router_id", info.mRouterId);
        add_rloc16(item, "rloc16", info.mRloc16);
        add_ext_address(item, "ext_address", &info.mExtAddress);
        cJSON_AddBoolToObject(item, "link_established", info.mLinkEstablished);
        cJSON_AddNumberToObject(item, "age", info.mAge);
        cJSON_AddNumberToObject(item, "version", info.mVersion);
        cJSON_AddNumberToObject(item, "next_hop_router_id", info.mNextHop);
        add_rloc16(item, "next_hop_rloc16", next_hop_rloc16);
        cJSON_AddNumberToObject(item, "path_cost", path_cost);
        cJSON_AddNumberToObject(item, "link_quality_in", info.mLinkQualityIn);
        cJSON_AddNumberToObject(item, "link_quality_out", info.mLinkQualityOut);
        cJSON_AddItemToArray(items, item);
    }
}

static void add_children_json_locked(cJSON *root, otInstance *instance)
{
    cJSON *items = cJSON_AddArrayToObject(root, "children");
    uint16_t max_children = otThreadGetMaxAllowedChildren(instance);

    for (uint16_t index = 0; index < max_children; ++index) {
        otChildInfo info = {0};
        if (otThreadGetChildInfoByIndex(instance, index, &info) != OT_ERROR_NONE) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "child_id", info.mChildId);
        add_rloc16(item, "rloc16", info.mRloc16);
        add_ext_address(item, "ext_address", &info.mExtAddress);
        cJSON_AddNumberToObject(item, "timeout", info.mTimeout);
        cJSON_AddNumberToObject(item, "age", info.mAge);
        cJSON_AddNumberToObject(item, "connection_time", info.mConnectionTime);
        cJSON_AddNumberToObject(item, "network_data_version", info.mNetworkDataVersion);
        cJSON_AddNumberToObject(item, "link_quality_in", info.mLinkQualityIn);
        cJSON_AddNumberToObject(item, "average_rssi", info.mAverageRssi);
        cJSON_AddNumberToObject(item, "last_rssi", info.mLastRssi);
        cJSON_AddNumberToObject(item, "queued_message_count", info.mQueuedMessageCnt);
        cJSON_AddBoolToObject(item, "rx_on_when_idle", info.mRxOnWhenIdle);
        cJSON_AddBoolToObject(item, "full_thread_device", info.mFullThreadDevice);
        cJSON_AddBoolToObject(item, "full_network_data", info.mFullNetworkData);
        cJSON_AddBoolToObject(item, "state_restoring", info.mIsStateRestoring);
        cJSON_AddBoolToObject(item, "csl_synced", info.mIsCslSynced);
        cJSON_AddItemToArray(items, item);
    }
}

static void router_diag_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "router diag worker started interval_ms=%u worker_period_ms=%u lock_timeout_ms=%u",
             (unsigned)CONFIG_PROBE_ROUTER_AUTO_SCAN_INTERVAL_MS,
             (unsigned)CONFIG_PROBE_ROUTER_SCAN_WORKER_PERIOD_MS,
             (unsigned)CONFIG_PROBE_ROUTER_WORKER_LOCK_TIMEOUT_MS);

    while (true) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance && esp_openthread_lock_acquire(pdMS_TO_TICKS(CONFIG_PROBE_ROUTER_WORKER_LOCK_TIMEOUT_MS))) {
            bool scan_stale = false;
            const uint32_t now = now_ms();
            const bool in_progress = router_diag_scan_in_progress(now, &scan_stale);

            if ((in_progress && scan_stale) ||
                (!in_progress && (uint32_t)(now - s_router_diag_last_auto_scan_ms) >= CONFIG_PROBE_ROUTER_AUTO_SCAN_INTERVAL_MS)) {
                bool restarted_stale = false;
                uint8_t sent = 0;
                if (start_router_diag_scan_with_lock(instance, &restarted_stale, &sent)) {
                    s_router_diag_last_auto_scan_ms = now;
                }
            }

            request_next_child_ext_address(instance);
            log_router_diag_progress();
            esp_openthread_lock_release();
        } else if (!instance) {
            ESP_LOGW(TAG, "router diag worker: openthread instance unavailable");
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_PROBE_ROUTER_SCAN_WORKER_PERIOD_MS));
    }
}

esp_err_t probe_thread_start_background_scan(void)
{
    if (s_router_diag_worker_started) {
        return ESP_OK;
    }

    s_router_diag_last_auto_scan_ms = now_ms() - CONFIG_PROBE_ROUTER_AUTO_SCAN_INTERVAL_MS;
    BaseType_t worker_ok = xTaskCreate(router_diag_worker_task, "router_diag_worker", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(worker_ok == pdPASS, ESP_FAIL, TAG, "failed to create router_diag_worker task");
    s_router_diag_worker_started = true;
    return ESP_OK;
}

cJSON *probe_thread_router_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        return root;
    }

    add_router_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_neighbors_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddArrayToObject(root, "neighbors");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "neighbors");
        return root;
    }

    add_neighbors_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_routers_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddArrayToObject(root, "routers");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "routers");
        return root;
    }

    add_routers_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_children_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    add_children_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_ipaddr_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddArrayToObject(root, "addresses");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "addresses");
        return root;
    }

    add_ipaddr_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_leader_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        return root;
    }

    add_leader_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_dataset_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        return root;
    }

    otOperationalDataset dataset = {0};
    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        return root;
    }

    if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE) {
        cJSON_AddBoolToObject(root, "configured", true);
        cJSON_AddStringToObject(root, "network_name", dataset.mNetworkName.m8);
        cJSON_AddNumberToObject(root, "channel", dataset.mChannel);
        cJSON_AddNumberToObject(root, "pan_id", dataset.mPanId);
        cJSON_AddBoolToObject(root, "has_network_key", dataset.mComponents.mIsNetworkKeyPresent);
        cJSON_AddBoolToObject(root, "has_mesh_local_prefix", dataset.mComponents.mIsMeshLocalPrefixPresent);
    } else {
        cJSON_AddBoolToObject(root, "configured", false);
    }
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_mesh_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        cJSON_AddArrayToObject(root, "neighbors");
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "neighbors");
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    add_router_json_locked(root, instance);
    cJSON *leader = cJSON_AddObjectToObject(root, "leader");
    add_leader_json_locked(leader, instance);
    add_neighbors_json_locked(root, instance);
    add_routers_json_locked(root, instance);
    add_children_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_topology_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        cJSON_AddArrayToObject(root, "routers");
        cJSON_AddArrayToObject(root, "neighbors");
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "state", "busy");
        cJSON_AddArrayToObject(root, "routers");
        cJSON_AddArrayToObject(root, "neighbors");
        cJSON_AddArrayToObject(root, "children");
        return root;
    }

    cJSON *self = cJSON_AddObjectToObject(root, "self");
    add_router_json_locked(self, instance);
    cJSON *leader = cJSON_AddObjectToObject(root, "leader");
    add_leader_json_locked(leader, instance);
    add_routers_json_locked(root, instance);
    add_neighbors_json_locked(root, instance);
    add_children_json_locked(root, instance);
    esp_openthread_lock_release();
    return root;
}

cJSON *probe_thread_router_neighbors_json(void)
{
    cJSON *root = cJSON_CreateObject();
    add_router_diag_snapshot_json(root);
    return root;
}

cJSON *probe_thread_router_neighbors_scan_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();
    bool scan_stale = false;
    uint8_t sent = 0;

    if (!instance) {
        cJSON_AddStringToObject(root, "status", "unavailable");
        return root;
    }

    if (!try_acquire_thread_lock()) {
        cJSON_AddStringToObject(root, "status", "busy");
        return root;
    }

    if (!start_router_diag_scan_with_lock(instance, &scan_stale, &sent)) {
        esp_openthread_lock_release();
        cJSON_AddStringToObject(root, "status", "scan_in_progress");
        add_router_diag_snapshot_json(root);
        return root;
    }

    s_router_diag_last_auto_scan_ms = now_ms();

    esp_openthread_lock_release();
    cJSON_AddStringToObject(root, "status", scan_stale ? "scan_restarted_stale" : "scan_started");
    cJSON_AddNumberToObject(root, "sent", sent);
    add_router_diag_snapshot_json(root);
    return root;
}

cJSON *probe_thread_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    otInstance *instance = esp_openthread_get_instance();

    if (!instance) {
        cJSON_AddBoolToObject(root, "thread_commissioned", false);
        cJSON_AddNullToObject(root, "thread_network_name");
        cJSON_AddStringToObject(root, "thread_role", "unavailable");
    } else {
        if (!try_acquire_thread_lock()) {
            cJSON_AddBoolToObject(root, "thread_commissioned", false);
            cJSON_AddNullToObject(root, "thread_network_name");
            cJSON_AddStringToObject(root, "thread_role", "busy");
            cJSON_AddItemToObject(root, "router", cJSON_CreateObject());
            cJSON_AddItemToObject(root, "ipaddr", cJSON_CreateObject());
            return root;
        }

        otOperationalDataset dataset = {0};
        otDeviceRole role = otThreadGetDeviceRole(instance);
        bool commissioned = otDatasetIsCommissioned(instance);

        cJSON_AddBoolToObject(root, "thread_commissioned", commissioned);
        cJSON_AddStringToObject(root, "thread_role", role_to_string(role));
        if (commissioned && otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE &&
            dataset.mComponents.mIsNetworkNamePresent) {
            cJSON_AddStringToObject(root, "thread_network_name", dataset.mNetworkName.m8);
        } else {
            cJSON_AddNullToObject(root, "thread_network_name");
        }
        cJSON *router = cJSON_AddObjectToObject(root, "router");
        add_router_json_locked(router, instance);
        cJSON *ipaddr = cJSON_AddObjectToObject(root, "ipaddr");
        add_ipaddr_json_locked(ipaddr, instance);
        esp_openthread_lock_release();
    }

    return root;
}
