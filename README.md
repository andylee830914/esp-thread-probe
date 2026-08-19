# ESP Thread Probe

`esp-thread-probe` is a dual-MCU Thread topology probe built with ESP-IDF.
An ESP32-C6 joins a Matter-over-Thread network, while an ESP-WROOM-32 provides
a Wi-Fi HTTP API for reading Thread telemetry from the C6 over UART.

The repository contains only the two firmware images needed for that final
hardware path:

- ESP32-C6 probe firmware in the repository root
- ESP-WROOM-32 uplink firmware in `firmware/uplink-wroom32`

It reports OpenThread role, leader, parent/router, neighbors, children, IPv6
addresses, route tables, router-neighbor diagnostics, and active dataset
summary.

## Status

This repository is an MVP scaffold. It is currently pinned for ESP-IDF 6.0.x
and tested with ESP-IDF 6.0.2. The ESP32-C6 firmware uses ESP-Matter for
Matter-over-Thread commissioning, while the ESP-WROOM-32 firmware owns Wi-Fi
and HTTP.

The split is intentional: the ESP32-C6 no longer keeps a Wi-Fi station link alive while Matter/OpenThread is active. This avoids the 2.4GHz coexistence failure mode where the AP drops the C6 while its local watchdog still believes Wi-Fi is connected.

## Hardware

- ESP32-C6 DevKitC or compatible ESP32-C6 board
- ESP-WROOM-32 / ESP32 DevKit board for Wi-Fi uplink
- 4MB flash or larger
- USB data cables for flashing both boards
- Existing Thread border router in the home, such as HomePod mini, Apple TV, Nest Hub, or another Matter Thread border router

## Requirements

- ESP-IDF 6.0.x, tested with ESP-IDF 6.0.2
- Python 3 from the active ESP-IDF environment
- Two serial ports, one for each board during flashing and debug
- Internet access during the first build so ESP-IDF Component Manager can fetch managed dependencies

## Quick Start

Activate ESP-IDF 6.0.2, then build and flash the ESP32-C6 probe:

```bash
cd <repo>
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
./scripts/build.sh
idf.py -p /dev/ttyACM0 flash monitor
```

Then build and flash the ESP-WROOM-32 uplink:

```bash
cd <repo>
cd firmware/uplink-wroom32
idf.py set-target esp32
cd ../..
./scripts/configure_uplink.py --wifi-ssid "YOUR_WIFI" --wifi-password "YOUR_PASSWORD"
cd firmware/uplink-wroom32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The helper updates the WROOM sdkconfig directly, so you do not need to find Wi-Fi settings in menuconfig.

After the WROOM gets an IP address, call the WROOM IP, not the C6:

```bash
curl http://WROOM_IP/health
curl http://WROOM_IP/mesh
curl http://WROOM_IP/router-neighbors/scan
sleep 2
curl http://WROOM_IP/router-neighbors
```

When the bridge is working, the C6 monitor logs lines like `probe_bridge: uart request: GET /health`.

If `/uplink` works but `/health` returns `probe timeout`, Wi-Fi and HTTP are fine and the issue is on the UART path. Check TX/RX crossing, common GND, and the configured GPIO pins.

## Architecture

```text
Apple Home / Home Assistant
        |
        | Matter over Thread
        v
ESP32-C6 Probe  <--- UART JSONL --->  ESP-WROOM-32 Uplink  <--- Wi-Fi HTTP ---> Browser / curl / HA
Thread + Matter                         Wi-Fi station + API
```

Default UART wiring:

| ESP32-C6 Probe | ESP-WROOM-32 Uplink |
| --- | --- |
| GPIO4 TX | GPIO16 RX |
| GPIO5 RX | GPIO17 TX |
| GND | GND |

Default UART speed is `115200`.

## API

These endpoints are served by the ESP-WROOM-32. The WROOM forwards probe
requests to the ESP32-C6 over UART and returns the C6 JSON response.

| Endpoint | Purpose |
| --- | --- |
| `GET /uplink` | WROOM-only local status, does not query the C6 |
| `GET /health` | Uplink/probe health |
| `GET /info` | Firmware, chip, uptime, feature flags |
| `GET /mesh` | Combined Thread network snapshot |
| `GET /neighbors` | Neighbor table |
| `GET /routers` | Thread router table with next hop and path cost |
| `GET /children` | Children attached to this probe when it is a parent |
| `GET /topology` | Combined self, leader, routers, neighbors, and local children |
| `GET /router-neighbors/scan` | Ask each Thread router for its router links using Network Diagnostic |
| `GET /router-neighbors` | Last cached router-neighbor diagnostic result |
| `GET /router` | Current role, RLOC16, router id, parent info |
| `GET /ipaddr` | Thread IPv6 addresses |
| `GET /leader` | Leader data |
| `GET /dataset` | Active dataset summary |

Example:

```json
{
  "state": "router",
  "parent": "0x5400",
  "leader": "0x1000",
  "neighbors": [],
  "children": []
}
```

## Matter Commissioning

The default Matter setup flow uses ESP-Matter over BLE. After flashing, watch the serial monitor for the onboarding codes:

- QR payload: `MT:Y.K9042C00KA0648G00`
- QR image URL: `https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:Y.K9042C00KA0648G00`
- Manual pairing code: `34970112332`

Scan the QR code with Apple Home, Home Assistant, or another Matter commissioner that already has access to your Thread network. The commissioner sends the Thread credentials to the ESP32-C6 over BLE during Matter commissioning. You do not need to manually set the Thread SSID, network name, or dataset for the normal Matter QR flow.

Only the ESP32-C6 is commissioned into Matter. The ESP-WROOM-32 is just a Wi-Fi API uplink and does not join Matter or Thread.

If you reflash without erasing NVS, the C6 keeps its Matter fabric and usually does not need to be added to Home again. If you erase flash/NVS, remove the old accessory from Home and commission it again.

The fixed passcode and discriminator are for development only. Production builds need unique factory data per device.

## Project Layout

```text
main/
components/
  probe_core/
  probe_matter/
firmware/
  uplink-wroom32/
patches/
  managed_components/
scripts/
docs/
.github/workflows/
```

## References

- ESP-IDF OpenThread documentation: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/network/esp_openthread.html
- ESP-IDF OpenThread guide: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/openthread.html
- ESP-Matter ESP32-C6 development guide: https://docs.espressif.com/projects/esp-matter/en/latest/esp32c6/developing.html
