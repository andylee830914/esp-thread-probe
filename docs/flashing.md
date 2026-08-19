# Flashing Guide

This project produces two firmware images:

- ESP32-C6 probe firmware from the repository root
- ESP-WROOM-32 uplink firmware from `firmware/uplink-wroom32`

## ESP32-C6 Probe

```bash
cd <repo>
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
./scripts/build.sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Linux serial ports usually look like `/dev/ttyACM0`.

## ESP-WROOM-32 Uplink

```bash
cd <repo>
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
cd firmware/uplink-wroom32
idf.py set-target esp32
cd ../..
./scripts/configure_uplink.py --wifi-ssid "YOUR_WIFI" --wifi-password "YOUR_PASSWORD"
cd firmware/uplink-wroom32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The helper writes Wi-Fi settings into the uplink project `sdkconfig`.

## Wiring

| ESP32-C6 Probe | ESP-WROOM-32 Uplink |
| --- | --- |
| GPIO4 TX | GPIO16 RX |
| GPIO5 RX | GPIO17 TX |
| GND | GND |

The default UART speed is `115200`.

## First Boot

Watch the ESP32-C6 serial monitor for the Matter onboarding lines:

- QR payload: `MT:Y.K9042C00KA0648G00`
- QR image URL: `https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:Y.K9042C00KA0648G00`
- Manual pairing code: `34970112332`

Open the QR image URL, scan it with Apple Home or Home Assistant, and add the probe as a Matter accessory. The commissioner sends Thread credentials to the device over BLE.

The default partition table expects 4MB flash or larger and uses one large app slot for this Matter MVP.

## Thread

Thread is normally configured by Matter commissioning. Do not look for a Thread SSID or password in menuconfig: Thread credentials come from Apple Home, Home Assistant, or another Matter commissioner when you scan the QR code.

After the WROOM gets an IP address, call the WROOM, not the C6:

```bash
curl http://<wroom-ip>/uplink
curl http://<wroom-ip>/health
curl http://<wroom-ip>/router
curl http://<wroom-ip>/mesh
```

If `/router` eventually reports `child`, `router`, or `leader`, Thread attached successfully.
