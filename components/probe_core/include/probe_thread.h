#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;
typedef struct otInstance otInstance;

esp_err_t probe_thread_start_background_scan(void);
esp_err_t probe_thread_apply_deterministic_ext_addr(otInstance *instance);
esp_err_t probe_thread_register_state_logger(otInstance *instance);
cJSON *probe_thread_info_json(void);
cJSON *probe_thread_mesh_json(void);
cJSON *probe_thread_neighbors_json(void);
cJSON *probe_thread_routers_json(void);
cJSON *probe_thread_children_json(void);
cJSON *probe_thread_topology_json(void);
cJSON *probe_thread_router_neighbors_json(void);
cJSON *probe_thread_router_neighbors_scan_json(void);
cJSON *probe_thread_router_json(void);
cJSON *probe_thread_ipaddr_json(void);
cJSON *probe_thread_leader_json(void);
cJSON *probe_thread_dataset_json(void);

#ifdef __cplusplus
}
#endif
