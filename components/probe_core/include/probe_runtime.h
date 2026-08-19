#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROBE_RUNTIME_BOOTING = 0,
    PROBE_RUNTIME_WIFI_HTTP_READY,
    PROBE_RUNTIME_MATTER_STARTING,
    PROBE_RUNTIME_MATTER_STARTED,
} probe_runtime_phase_t;

void probe_runtime_set_phase(probe_runtime_phase_t phase);
probe_runtime_phase_t probe_runtime_get_phase(void);
const char *probe_runtime_phase_name(probe_runtime_phase_t phase);
bool probe_runtime_matter_started(void);

#ifdef __cplusplus
}
#endif
