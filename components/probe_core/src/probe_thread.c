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
#include "openthread/mesh_diag.h"
#include "openthread/netdiag.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

static const char *TAG = "probe_thread";

#define PROBE_MAX_ROUTER_DIAG_ENTRIES 64
#define PROBE_MAX_ROUTER_LINKS 16
#define PROBE_MAX_ROUTER_CHILDREN 16

typedef struct {
    uint8_t router_id;
    uint16_t rloc16;
    uint8_t link_quality;
    bool has_ext_address;
    otExtAddress ext_address;
    uint16_t version;
    uint8_t link_margin;
    int8_t average_rssi;
    int8_t last_rssi;
    uint32_t connection_time;
    uint16_t frame_error_rate;
    uint16_t message_error_rate;
    bool supports_error_rate;
    bool has_link;
    bool has_link_quality;
    bool has_detail;
} probe_router_link_t;

typedef struct {
    uint16_t rloc16;
    uint32_t timeout;
    uint32_t age;
    uint32_t connection_time;
    uint16_t version;
    uint16_t supervision_interval;
    uint8_t link_margin;
    int8_t average_rssi;
    int8_t last_rssi;
    uint16_t frame_error_rate;
    uint16_t message_error_rate;
    uint16_t queued_message_count;
    uint16_t csl_period;
    uint32_t csl_timeout;
    uint8_t csl_channel;
    uint8_t link_quality;
    otLinkModeConfig mode;
    bool rx_on_when_idle;
    bool device_type_ftd;
    bool full_net_data;
    bool csl_synchronized;
    bool supports_error_rate;
    bool is_this_device;
    bool is_border_router;
    bool has_ext_address;
    bool has_link_quality;
    bool has_detail;
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
    uint16_t version;
    uint8_t child_count;
    uint8_t stored_child_count;
    uint8_t link_count;
    bool valid;
    bool pending;
    bool responded;
    bool topology_responded;
    bool detail_responded;
    bool is_this_device;
    bool is_this_device_parent;
    bool is_leader;
    bool is_border_router;
    bool child_table_pending;
    bool child_table_done;
    bool router_neighbor_pending;
    bool router_neighbor_done;
    otError child_table_error;
    otError router_neighbor_error;
    otError error;
    probe_router_link_t links[PROBE_MAX_ROUTER_LINKS];
    probe_router_child_t children[PROBE_MAX_ROUTER_CHILDREN];
} probe_router_diag_entry_t;

typedef struct {
    uint8_t router_id;
    uint16_t rloc16;
} probe_router_diag_context_t;

typedef enum {
    PROBE_MESH_DIAG_QUERY_NONE = 0,
    PROBE_MESH_DIAG_QUERY_CHILD_TABLE,
    PROBE_MESH_DIAG_QUERY_ROUTER_NEIGHBOR_TABLE,
} probe_mesh_diag_query_type_t;

static portMUX_TYPE s_router_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static probe_router_diag_entry_t s_router_diag_entries[PROBE_MAX_ROUTER_DIAG_ENTRIES];
static probe_router_diag_context_t s_router_diag_contexts[PROBE_MAX_ROUTER_DIAG_ENTRIES];
static uint32_t s_router_diag_scan_started_ms;
static uint32_t s_router_diag_scan_id;
static uint8_t s_router_diag_pending_count;
static uint32_t s_router_diag_last_auto_scan_ms;
static bool s_router_diag_worker_started;
static bool s_mesh_diag_topology_pending;
static probe_mesh_diag_query_type_t s_mesh_diag_query_type;
static probe_router_diag_context_t s_mesh_diag_query_context;

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

static bool thread_is_attached(otInstance *instance)
{
    return role_is_attached(otThreadGetDeviceRole(instance));
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
static probe_thread_attach_callback_t s_thread_attach_callback;
static void *s_thread_attach_callback_context;

void probe_thread_set_attach_callback(probe_thread_attach_callback_t callback, void *context)
{
    s_thread_attach_callback = callback;
    s_thread_attach_callback_context = context;
}

static void notify_thread_attach_state(bool attached)
{
    if (s_thread_attach_callback) {
        s_thread_attach_callback(attached, s_thread_attach_callback_context);
    }
}

static void thread_state_changed_cb(otChangedFlags flags, void *context)
{
    if ((flags & OT_CHANGED_THREAD_ROLE) == 0) {
        return;
    }

    otInstance *instance = (otInstance *)context;
    bool attached = thread_is_attached(instance);

    if (attached && !s_thread_was_attached) {
        log_thread_attach_state(instance);
    }
    if (attached != s_thread_was_attached) {
        notify_thread_attach_state(attached);
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

    s_thread_was_attached = thread_is_attached(instance);
    if (s_thread_was_attached) {
        log_thread_attach_state(instance);
    }
    notify_thread_attach_state(s_thread_was_attached);

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

static bool ext_address_is_zero(const otExtAddress *addr)
{
    for (size_t i = 0; i < sizeof(addr->m8); ++i) {
        if (addr->m8[i] != 0) {
            return false;
        }
    }
    return true;
}

static void set_ext_address_if_valid(bool *has_ext_address, otExtAddress *dest, const otExtAddress *src)
{
    if (!ext_address_is_zero(src)) {
        *has_ext_address = true;
        *dest = *src;
    }
}

static void add_number_or_null(cJSON *obj, const char *name, bool valid, double value)
{
    if (valid) {
        cJSON_AddNumberToObject(obj, name, value);
    }
}

static void add_bool_or_null(cJSON *obj, const char *name, bool valid, bool value)
{
    if (valid) {
        cJSON_AddBoolToObject(obj, name, value);
    }
}

static void add_error_rate_json(cJSON *obj, const char *raw_name, const char *percent_name, bool valid, uint16_t value)
{
    (void)percent_name;
    add_number_or_null(obj, raw_name, valid, value);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static probe_router_diag_entry_t *diag_entry_by_router_id(uint8_t router_id)
{
    if (router_id >= PROBE_MAX_ROUTER_DIAG_ENTRIES) {
        return NULL;
    }
    return &s_router_diag_entries[router_id];
}

static bool is_router_rloc16(uint16_t rloc16)
{
    return (rloc16 & 0x03ff) == 0 && (rloc16 >> 10) <= OT_NETWORK_MAX_ROUTER_ID;
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

static const char *child_ext_address_status(const probe_router_diag_entry_t *entry,
                                            const probe_router_child_t *child)
{
    if (child->has_ext_address) {
        return "ok";
    }
    if (entry->child_table_pending) {
        return "pending";
    }
    if (!entry->child_table_done && entry->error == OT_ERROR_NONE) {
        return "queued";
    }
    if (entry->child_table_done) {
        return entry->child_table_error == OT_ERROR_NONE ? "unavailable" : ot_error_name(entry->child_table_error);
    }
    return ot_error_name(entry->error);
}

static const char *detail_query_status(bool pending, bool done, otError error)
{
    if (pending) {
        return "pending";
    }
    if (!done) {
        return "queued";
    }
    return ot_error_name(error);
}

static probe_router_child_t *find_or_add_router_child(probe_router_diag_entry_t *entry, uint16_t rloc16)
{
    for (uint8_t i = 0; i < entry->stored_child_count; ++i) {
        if (entry->children[i].rloc16 == rloc16) {
            return &entry->children[i];
        }
    }

    if (entry->stored_child_count >= PROBE_MAX_ROUTER_CHILDREN) {
        return NULL;
    }

    probe_router_child_t *child = &entry->children[entry->stored_child_count++];
    memset(child, 0, sizeof(*child));
    child->rloc16 = rloc16;
    return child;
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
    link->rloc16 = (uint16_t)router_id << 10;
    return link;
}

static void seed_router_diag_entries_from_router_table(otInstance *instance)
{
    uint8_t max_router_id = otThreadGetMaxRouterId(instance);

    for (uint8_t router_id = 0; router_id <= max_router_id; ++router_id) {
        otRouterInfo info = {0};
        if (otThreadGetRouterInfo(instance, router_id, &info) != OT_ERROR_NONE || !info.mAllocated) {
            continue;
        }

        taskENTER_CRITICAL(&s_router_diag_lock);
        probe_router_diag_entry_t *entry = diag_entry_by_router_id(info.mRouterId);
        if (entry) {
            entry->router_id = info.mRouterId;
            entry->rloc16 = info.mRloc16;
            entry->version = info.mVersion;
            entry->scan_id = s_router_diag_scan_id;
            entry->updated_ms = now_ms();
            entry->valid = true;
            entry->pending = true;
            entry->responded = false;
            entry->error = OT_ERROR_NONE;
            set_ext_address_if_valid(&entry->has_ext_address, &entry->ext_address, &info.mExtAddress);
        }
        taskEXIT_CRITICAL(&s_router_diag_lock);
    }
}

static void store_topology_child_info(probe_router_diag_entry_t *entry, otMeshDiagRouterInfo *router_info)
{
    if (!router_info || !router_info->mChildIterator) {
        return;
    }

    otMeshDiagChildInfo info = {0};
    while (otMeshDiagGetNextChildInfo(router_info->mChildIterator, &info) == OT_ERROR_NONE) {
        probe_router_child_t *child = find_or_add_router_child(entry, info.mRloc16);
        if (!child) {
            continue;
        }
        child->mode = info.mMode;
        child->link_quality = info.mLinkQuality;
        child->has_link_quality = true;
        child->is_this_device = info.mIsThisDevice;
        child->is_border_router = info.mIsBorderRouter;
        child->rx_on_when_idle = info.mMode.mRxOnWhenIdle;
        child->device_type_ftd = info.mMode.mDeviceType;
        child->full_net_data = info.mMode.mNetworkData;
    }
}

static void store_topology_router_info(otMeshDiagRouterInfo *router_info, otError error)
{
    if (!router_info) {
        return;
    }

    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(router_info->mRouterId);
    if (entry) {
        bool had_ext_address = entry->has_ext_address;
        otExtAddress ext_address = entry->ext_address;

        memset(entry, 0, sizeof(*entry));
        entry->router_id = router_info->mRouterId;
        entry->rloc16 = router_info->mRloc16;
        entry->version = router_info->mVersion;
        entry->scan_id = s_router_diag_scan_id;
        entry->updated_ms = now_ms();
        entry->valid = true;
        entry->pending = true;
        entry->responded = error == OT_ERROR_PENDING || error == OT_ERROR_NONE;
        entry->topology_responded = entry->responded;
        entry->error = OT_ERROR_NONE;
        entry->has_ext_address = had_ext_address;
        entry->ext_address = ext_address;
        set_ext_address_if_valid(&entry->has_ext_address, &entry->ext_address, &router_info->mExtAddress);
        entry->is_this_device = router_info->mIsThisDevice;
        entry->is_this_device_parent = router_info->mIsThisDeviceParent;
        entry->is_leader = router_info->mIsLeader;
        entry->is_border_router = router_info->mIsBorderRouter;

        for (uint8_t router_id = 0; router_id <= OT_NETWORK_MAX_ROUTER_ID; ++router_id) {
            uint8_t link_quality = router_info->mLinkQualities[router_id];
            if (router_id == router_info->mRouterId || link_quality == 0) {
                continue;
            }
            probe_router_link_t *link = find_or_add_router_link(entry, router_id);
            if (!link) {
                continue;
            }
            link->has_link = true;
            link->link_quality = link_quality;
            link->has_link_quality = true;
        }

        store_topology_child_info(entry, router_info);
        entry->child_count = entry->stored_child_count;
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
}

static void mesh_diag_topology_callback(otError error, otMeshDiagRouterInfo *router_info, void *context)
{
    (void)context;

    store_topology_router_info(router_info, error);

    if (error != OT_ERROR_PENDING) {
        taskENTER_CRITICAL(&s_router_diag_lock);
        s_mesh_diag_topology_pending = false;
        if (s_router_diag_pending_count > 0) {
            s_router_diag_pending_count--;
        }
        for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
            probe_router_diag_entry_t *entry = &s_router_diag_entries[i];
            if (entry->scan_id == s_router_diag_scan_id && entry->valid) {
                entry->pending = false;
            }
        }
        taskEXIT_CRITICAL(&s_router_diag_lock);
    }
}

static void mesh_diag_child_table_callback(otError error, const otMeshDiagChildEntry *child_entry, void *context)
{
    probe_router_diag_context_t *query = (probe_router_diag_context_t *)context;
    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(query ? query->router_id : 0xff);
    if (entry) {
        if (error == OT_ERROR_PENDING && child_entry) {
            probe_router_child_t *child = find_or_add_router_child(entry, child_entry->mRloc16);
            if (child) {
                entry->responded = true;
                entry->detail_responded = true;
                child->has_detail = true;
                child->rloc16 = child_entry->mRloc16;
                child->timeout = child_entry->mTimeout;
                child->age = child_entry->mAge;
                child->connection_time = child_entry->mConnectionTime;
                child->version = child_entry->mVersion;
                child->supervision_interval = child_entry->mSupervisionInterval;
                child->link_margin = child_entry->mLinkMargin;
                child->average_rssi = child_entry->mAverageRssi;
                child->last_rssi = child_entry->mLastRssi;
                child->frame_error_rate = child_entry->mFrameErrorRate;
                child->message_error_rate = child_entry->mMessageErrorRate;
                child->queued_message_count = child_entry->mQueuedMessageCount;
                child->csl_period = child_entry->mCslPeriod;
                child->csl_timeout = child_entry->mCslTimeout;
                child->csl_channel = child_entry->mCslChannel;
                child->rx_on_when_idle = child_entry->mRxOnWhenIdle;
                child->device_type_ftd = child_entry->mDeviceTypeFtd;
                child->full_net_data = child_entry->mFullNetData;
                child->csl_synchronized = child_entry->mCslSynchronized;
                child->supports_error_rate = child_entry->mSupportsErrRate;
                set_ext_address_if_valid(&child->has_ext_address, &child->ext_address, &child_entry->mExtAddress);
                child->mode.mRxOnWhenIdle = child_entry->mRxOnWhenIdle;
                child->mode.mDeviceType = child_entry->mDeviceTypeFtd;
                child->mode.mNetworkData = child_entry->mFullNetData;
            }
        } else {
            entry->child_table_pending = false;
            entry->child_table_done = true;
            entry->child_table_error = error;
            entry->pending = false;
            entry->updated_ms = now_ms();
            s_mesh_diag_query_type = PROBE_MESH_DIAG_QUERY_NONE;
            if (s_router_diag_pending_count > 0) {
                s_router_diag_pending_count--;
            }
        }
        entry->child_count = entry->stored_child_count;
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
}

static void mesh_diag_router_neighbor_table_callback(otError error,
                                                     const otMeshDiagRouterNeighborEntry *neighbor_entry,
                                                     void *context)
{
    probe_router_diag_context_t *query = (probe_router_diag_context_t *)context;
    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(query ? query->router_id : 0xff);
    if (entry) {
        if (error == OT_ERROR_PENDING && neighbor_entry && is_router_rloc16(neighbor_entry->mRloc16)) {
            uint8_t neighbor_router_id = (uint8_t)(neighbor_entry->mRloc16 >> 10);
            probe_router_link_t *link = find_or_add_router_link(entry, neighbor_router_id);
            if (link) {
                entry->responded = true;
                entry->detail_responded = true;
                link->has_link = true;
                link->has_detail = true;
                link->rloc16 = neighbor_entry->mRloc16;
                set_ext_address_if_valid(&link->has_ext_address, &link->ext_address, &neighbor_entry->mExtAddress);
                link->version = neighbor_entry->mVersion;
                link->connection_time = neighbor_entry->mConnectionTime;
                link->link_margin = neighbor_entry->mLinkMargin;
                link->average_rssi = neighbor_entry->mAverageRssi;
                link->last_rssi = neighbor_entry->mLastRssi;
                link->supports_error_rate = neighbor_entry->mSupportsErrRate;
                link->frame_error_rate = neighbor_entry->mFrameErrorRate;
                link->message_error_rate = neighbor_entry->mMessageErrorRate;

                probe_router_diag_entry_t *neighbor = diag_entry_by_router_id(neighbor_router_id);
                if (neighbor) {
                    set_ext_address_if_valid(&neighbor->has_ext_address,
                                             &neighbor->ext_address,
                                             &neighbor_entry->mExtAddress);
                }
            }
        } else {
            entry->router_neighbor_pending = false;
            entry->router_neighbor_done = true;
            entry->router_neighbor_error = error;
            entry->pending = false;
            entry->updated_ms = now_ms();
            s_mesh_diag_query_type = PROBE_MESH_DIAG_QUERY_NONE;
            if (s_router_diag_pending_count > 0) {
                s_router_diag_pending_count--;
            }
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
}

static void add_router_diag_entry_json(cJSON *routers, const probe_router_diag_entry_t *entry)
{
    cJSON *router = cJSON_CreateObject();
    cJSON_AddNumberToObject(router, "router_id", entry->router_id);
    add_rloc16(router, "rloc16", entry->rloc16);
    cJSON_AddStringToObject(router, "status", entry->pending ? "pending" : ot_error_name(entry->error));
    cJSON_AddBoolToObject(router, "responded", entry->responded);
    cJSON_AddBoolToObject(router, "topology_responded", entry->topology_responded);
    cJSON_AddBoolToObject(router, "detail_responded", entry->detail_responded);
    if (entry->has_ext_address) {
        add_ext_address(router, "ext_address", &entry->ext_address);
    } else {
        cJSON_AddNullToObject(router, "ext_address");
    }
    cJSON_AddNumberToObject(router, "updated_ms", entry->updated_ms);
    cJSON_AddNumberToObject(router, "version", entry->version);
    cJSON_AddBoolToObject(router, "leader", entry->is_leader);
    cJSON_AddBoolToObject(router, "border_router", entry->is_border_router);
    cJSON_AddBoolToObject(router, "this_device", entry->is_this_device);
    cJSON_AddBoolToObject(router, "this_device_parent", entry->is_this_device_parent);
    cJSON_AddNumberToObject(router, "child_count", entry->child_count);
    cJSON_AddBoolToObject(router, "child_table_done", entry->child_table_done);
    cJSON_AddStringToObject(router,
                            "child_table_status",
                            detail_query_status(entry->child_table_pending,
                                                entry->child_table_done,
                                                entry->child_table_error));
    cJSON_AddBoolToObject(router, "router_neighbor_table_done", entry->router_neighbor_done);
    cJSON_AddStringToObject(router,
                            "router_neighbor_table_status",
                            detail_query_status(entry->router_neighbor_pending,
                                                entry->router_neighbor_done,
                                                entry->router_neighbor_error));
    cJSON *links = cJSON_AddArrayToObject(router, "router_neighbors");
    for (uint8_t j = 0; j < entry->link_count; ++j) {
        const probe_router_link_t *link = &entry->links[j];
        const probe_router_diag_entry_t *linked_router = diag_entry_by_router_id(link->router_id);
        const otExtAddress *ext_address = NULL;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "router_id", link->router_id);
        if (link->has_ext_address) {
            ext_address = &link->ext_address;
        } else if (linked_router && linked_router->has_ext_address) {
            ext_address = &linked_router->ext_address;
        }
        add_rloc16(item, "rloc16", link->rloc16 != 0 ? link->rloc16 : (uint16_t)(link->router_id << 10));
        if (ext_address) {
            add_ext_address(item, "ext_address", ext_address);
        } else {
            cJSON_AddNullToObject(item, "ext_address");
        }
        add_number_or_null(item, "link_quality", link->has_link_quality, link->link_quality);
        add_number_or_null(item, "version", link->has_detail, link->version);
        add_number_or_null(item, "link_margin", link->has_detail, link->link_margin);
        add_number_or_null(item, "average_rssi", link->has_detail, link->average_rssi);
        add_number_or_null(item, "last_rssi", link->has_detail, link->last_rssi);
        add_number_or_null(item, "connection_time", link->has_detail, link->connection_time);
        add_bool_or_null(item, "supports_error_rate", link->has_detail, link->supports_error_rate);
        add_error_rate_json(item,
                            "frame_error_rate",
                            "frame_error_rate_percent",
                            link->has_detail && link->supports_error_rate,
                            link->frame_error_rate);
        add_error_rate_json(item,
                            "message_error_rate",
                            "message_error_rate_percent",
                            link->has_detail && link->supports_error_rate,
                            link->message_error_rate);
        cJSON_AddItemToArray(links, item);
    }
    cJSON *children = cJSON_AddArrayToObject(router, "children");
    for (uint8_t j = 0; j < entry->stored_child_count; ++j) {
        const probe_router_child_t *child = &entry->children[j];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "child_id", child->rloc16 & 0x01ff);
        add_rloc16(item, "rloc16", child->rloc16);
        add_number_or_null(item, "timeout", child->has_detail, child->timeout);
        add_number_or_null(item, "age", child->has_detail, child->age);
        add_number_or_null(item, "connection_time", child->has_detail, child->connection_time);
        add_number_or_null(item, "version", child->has_detail, child->version);
        add_number_or_null(item, "link_margin", child->has_detail, child->link_margin);
        add_number_or_null(item, "average_rssi", child->has_detail, child->average_rssi);
        add_number_or_null(item, "last_rssi", child->has_detail, child->last_rssi);
        add_number_or_null(item, "queued_message_count", child->has_detail, child->queued_message_count);
        add_number_or_null(item, "supervision_interval", child->has_detail, child->supervision_interval);
        add_bool_or_null(item, "supports_error_rate", child->has_detail, child->supports_error_rate);
        add_error_rate_json(item,
                            "frame_error_rate",
                            "frame_error_rate_percent",
                            child->has_detail && child->supports_error_rate,
                            child->frame_error_rate);
        add_error_rate_json(item,
                            "message_error_rate",
                            "message_error_rate_percent",
                            child->has_detail && child->supports_error_rate,
                            child->message_error_rate);
        add_bool_or_null(item, "csl_synchronized", child->has_detail, child->csl_synchronized);
        add_number_or_null(item, "csl_period", child->has_detail, child->csl_period);
        add_number_or_null(item, "csl_timeout", child->has_detail, child->csl_timeout);
        add_number_or_null(item, "csl_channel", child->has_detail, child->csl_channel);
        add_number_or_null(item, "link_quality", child->has_link_quality, child->link_quality);
        if (child->has_ext_address) {
            add_ext_address(item, "ext_address", &child->ext_address);
        } else {
            cJSON_AddNullToObject(item, "ext_address");
        }
        cJSON_AddStringToObject(item, "ext_address_status", child_ext_address_status(entry, child));
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
    uint16_t active_detail_query = 0;
    uint16_t remaining_detail_query = 0;
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
        if (entry.child_table_pending || entry.router_neighbor_pending) {
            active_detail_query++;
        }
        if (!entry.child_table_done) {
            remaining_detail_query++;
        }
        if (!entry.router_neighbor_done) {
            remaining_detail_query++;
        }
        if (!entry.child_table_done || entry.child_table_pending) {
            for (uint8_t j = 0; j < entry.stored_child_count; ++j) {
                if (!entry.children[j].has_ext_address) {
                    pending_child_mac++;
                }
            }
        }
        add_router_diag_entry_json(routers, &entry);
    }

    cJSON_AddNumberToObject(root, "pending", pending_router);
    cJSON_AddNumberToObject(root, "pending_router", pending_router);
    cJSON_AddNumberToObject(root, "active_detail_query", active_detail_query);
    cJSON_AddNumberToObject(root, "pending_detail_query", remaining_detail_query);
    cJSON_AddNumberToObject(root, "pending_child_mac", pending_child_mac);
    cJSON_AddNumberToObject(root, "pending_total", (uint32_t)pending_router + remaining_detail_query);
}

static bool router_diag_scan_in_progress(uint32_t current_ms, bool *scan_stale)
{
    bool in_progress = false;
    bool stale = false;
    uint32_t scan_started_ms = 0;

    taskENTER_CRITICAL(&s_router_diag_lock);
    scan_started_ms = s_router_diag_scan_started_ms;
    in_progress = s_mesh_diag_topology_pending || s_router_diag_pending_count > 0;
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
    static uint16_t s_last_pending_detail = 0xffff;
    static uint8_t s_last_done_router = 0xff;
    static uint8_t s_last_total_router = 0xff;
    static uint32_t s_last_log_ms = 0;

    uint32_t scan_id = 0;
    uint8_t pending_router = 0;
    uint16_t pending_detail_query = 0;
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
        if (entry->child_table_pending || entry->router_neighbor_pending) {
            pending_detail_query++;
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    const uint32_t now = now_ms();
    const bool changed = scan_id != s_last_log_scan_id ||
                         pending_router != s_last_pending_router ||
                         pending_detail_query != s_last_pending_detail ||
                         done_router != s_last_done_router ||
                         total_router != s_last_total_router;

    if (changed || (now - s_last_log_ms) >= CONFIG_PROBE_ROUTER_PROGRESS_LOG_INTERVAL_MS) {
        ESP_LOGI(TAG,
                 "router scan progress scan_id=%lu routers=%u/%u pending_router=%u pending_detail_query=%u",
                 (unsigned long)scan_id,
                 done_router,
                 total_router,
                 pending_router,
                 (unsigned)pending_detail_query);
        s_last_log_scan_id = scan_id;
        s_last_pending_router = pending_router;
        s_last_pending_detail = pending_detail_query;
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
    if (!thread_is_attached(instance)) {
        return false;
    }

    taskENTER_CRITICAL(&s_router_diag_lock);
    memset(s_router_diag_entries, 0, sizeof(s_router_diag_entries));
    memset(s_router_diag_contexts, 0, sizeof(s_router_diag_contexts));
    s_router_diag_scan_id++;
    s_router_diag_scan_started_ms = now_ms();
    s_router_diag_pending_count = 1;
    s_mesh_diag_topology_pending = true;
    s_mesh_diag_query_type = PROBE_MESH_DIAG_QUERY_NONE;
    taskEXIT_CRITICAL(&s_router_diag_lock);

    seed_router_diag_entries_from_router_table(instance);

    otMeshDiagDiscoverConfig config = {
        .mDiscoverIp6Addresses = false,
        .mDiscoverChildTable = true,
    };
    otError err = otMeshDiagDiscoverTopology(instance, &config, mesh_diag_topology_callback, NULL);
    if (err != OT_ERROR_NONE) {
        taskENTER_CRITICAL(&s_router_diag_lock);
        s_mesh_diag_topology_pending = false;
        s_router_diag_pending_count = 0;
        taskEXIT_CRITICAL(&s_router_diag_lock);
        return false;
    }

    if (sent) {
        *sent = 1;
    }
    return true;
}

static void request_next_mesh_diag_detail(otInstance *instance)
{
    probe_router_diag_context_t query = {0};
    probe_mesh_diag_query_type_t query_type = PROBE_MESH_DIAG_QUERY_NONE;

    taskENTER_CRITICAL(&s_router_diag_lock);
    if (s_mesh_diag_topology_pending || s_mesh_diag_query_type != PROBE_MESH_DIAG_QUERY_NONE) {
        taskEXIT_CRITICAL(&s_router_diag_lock);
        return;
    }

    for (uint8_t i = 0; i < PROBE_MAX_ROUTER_DIAG_ENTRIES; ++i) {
        probe_router_diag_entry_t *entry = &s_router_diag_entries[i];
        if (entry->scan_id != s_router_diag_scan_id || !entry->valid || entry->error != OT_ERROR_NONE) {
            continue;
        }
        if (!entry->child_table_done && !entry->child_table_pending) {
            entry->pending = true;
            entry->child_table_pending = true;
            query_type = PROBE_MESH_DIAG_QUERY_CHILD_TABLE;
        } else if (!entry->router_neighbor_done && !entry->router_neighbor_pending) {
            entry->pending = true;
            entry->router_neighbor_pending = true;
            query_type = PROBE_MESH_DIAG_QUERY_ROUTER_NEIGHBOR_TABLE;
        }

        if (query_type != PROBE_MESH_DIAG_QUERY_NONE) {
            query.router_id = entry->router_id;
            query.rloc16 = entry->rloc16;
            s_mesh_diag_query_type = query_type;
            s_mesh_diag_query_context = query;
            s_router_diag_pending_count++;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);

    if (query_type == PROBE_MESH_DIAG_QUERY_NONE) {
        return;
    }

    otError err = OT_ERROR_NONE;
    if (query_type == PROBE_MESH_DIAG_QUERY_CHILD_TABLE) {
        err = otMeshDiagQueryChildTable(instance, query.rloc16, mesh_diag_child_table_callback, &s_mesh_diag_query_context);
    } else {
        err = otMeshDiagQueryRouterNeighborTable(instance,
                                                 query.rloc16,
                                                 mesh_diag_router_neighbor_table_callback,
                                                 &s_mesh_diag_query_context);
    }

    if (err == OT_ERROR_NONE) {
        return;
    }

    taskENTER_CRITICAL(&s_router_diag_lock);
    probe_router_diag_entry_t *entry = diag_entry_by_router_id(query.router_id);
    if (entry) {
        entry->pending = false;
        entry->updated_ms = now_ms();
        if (query_type == PROBE_MESH_DIAG_QUERY_CHILD_TABLE) {
            entry->child_table_pending = false;
            entry->child_table_done = true;
            entry->child_table_error = err;
        } else {
            entry->router_neighbor_pending = false;
            entry->router_neighbor_done = true;
            entry->router_neighbor_error = err;
        }
    }
    s_mesh_diag_query_type = PROBE_MESH_DIAG_QUERY_NONE;
    if (s_router_diag_pending_count > 0) {
        s_router_diag_pending_count--;
    }
    taskEXIT_CRITICAL(&s_router_diag_lock);
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
            if (thread_is_attached(instance)) {
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

                request_next_mesh_diag_detail(instance);
                log_router_diag_progress();
            }
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

    if (!thread_is_attached(instance)) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
        cJSON_AddStringToObject(root, "status", "detached");
        cJSON_AddStringToObject(root, "thread_role", role_to_string(role));
        add_router_diag_snapshot_json(root);
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
