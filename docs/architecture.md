# Architecture

ESP Thread Probe (`esp-thread-probe`) is split into two firmware images.

## ESP32-C6 Probe

The root project builds the ESP32-C6 firmware. It owns Matter-over-Thread
commissioning, OpenThread telemetry, and the UART JSON-lines bridge.

## `probe_core`

Owns runtime behavior that should remain stable:

- lock-safe OpenThread telemetry reads
- Thread topology serialization
- UART JSON-lines command bridge

## `probe_matter`

Owns Matter SDK integration and OpenThread stack startup.

Keeping this separate makes ESP-Matter upgrades less invasive. ESP-Matter setup code tends to change more often than telemetry collection code.

## ESP-WROOM-32 Uplink

`firmware/uplink-wroom32` builds the ESP32/WROOM firmware. It owns Wi-Fi and the
HTTP JSON API, then forwards each request to the ESP32-C6 over UART.

## Data Flow

```text
HTTP client -> WROOM HTTP server -> UART JSONL -> C6 probe_bridge
                                              -> probe_thread -> cJSON
                                              -> UART JSONL -> WROOM response
```
