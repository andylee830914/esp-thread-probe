#include "probe_runtime.h"

static volatile probe_runtime_phase_t s_phase = PROBE_RUNTIME_BOOTING;

void probe_runtime_set_phase(probe_runtime_phase_t phase)
{
    s_phase = phase;
}

probe_runtime_phase_t probe_runtime_get_phase(void)
{
    return s_phase;
}

const char *probe_runtime_phase_name(probe_runtime_phase_t phase)
{
    switch (phase) {
    case PROBE_RUNTIME_BOOTING:
        return "booting";
    case PROBE_RUNTIME_WIFI_HTTP_READY:
        return "wifi_http_ready";
    case PROBE_RUNTIME_MATTER_STARTING:
        return "matter_starting";
    case PROBE_RUNTIME_MATTER_STARTED:
        return "matter_started";
    default:
        return "unknown";
    }
}

bool probe_runtime_matter_started(void)
{
    return s_phase == PROBE_RUNTIME_MATTER_STARTED;
}
