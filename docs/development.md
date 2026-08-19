# Development Notes

## ESP-IDF

This project currently targets ESP-IDF 6.0.2, with an ESP32-C6 probe firmware
and an ESP32/WROOM uplink firmware.

ESP-IDF 6.x moved cJSON out of the built-in `json` component. The `probe_core` component declares `espressif/cjson` in `components/probe_core/idf_component.yml` for IDF 6.x builds.

The ESP32-C6 application entry point is the repository root project. Its layout
keeps ESP-IDF application code at the root while isolating probe logic in
`components/probe_core` and Matter-specific integration in `components/probe_matter`.

Generated `managed_components/` directories are ignored. Any required local
changes to downloaded dependencies must be committed as patches under
`patches/managed_components` and applied with
`scripts/apply_managed_component_patches.py`.

The default partition table uses one large app slot on 4MB flash. Matter plus Thread is too large for two OTA slots on 4MB in the current MVP.

The OpenThread APIs are called under `esp_openthread_lock_acquire()` because OpenThread APIs are not thread-safe across tasks.

## Matter

Matter integration lives behind `components/probe_matter`.

The current MVP:

- adds ESP-Matter as a managed component
- creates a simple root node and On/Off endpoint for commissioning
- publishes the Matter setup payload over serial logs
- lets Matter own OpenThread startup
- keeps JSON telemetry independent from Matter endpoint logic

## Telemetry Shape

The stable public shape should be `/mesh`. Other endpoints may grow more detailed fields without breaking dashboards.
