#!/usr/bin/env python3
import argparse
from pathlib import Path


CONFIG_KEYS = {
    "wifi_ssid": "CONFIG_UPLINK_WIFI_SSID",
    "wifi_password": "CONFIG_UPLINK_WIFI_PASSWORD",
    "uart_tx": "CONFIG_UPLINK_UART_TX_GPIO",
    "uart_rx": "CONFIG_UPLINK_UART_RX_GPIO",
    "uart_baud": "CONFIG_UPLINK_UART_BAUD",
    "http_port": "CONFIG_UPLINK_HTTP_PORT",
}


def quote(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def set_key(lines: list[str], key: str, value: str, string_value: bool) -> list[str]:
    rendered = quote(value) if string_value else value
    assignment = f"{key}={rendered}\n"
    for index, line in enumerate(lines):
        if line.startswith(f"{key}="):
            lines[index] = assignment
            return lines
    lines.append(assignment)
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description="Configure the ESP-WROOM-32 uplink sdkconfig.")
    parser.add_argument("--wifi-ssid", required=True, help="Wi-Fi SSID for the HTTP API uplink")
    parser.add_argument("--wifi-password", required=True, help="Wi-Fi password")
    parser.add_argument("--uart-tx", default=None, help="WROOM TX GPIO connected to C6 RX")
    parser.add_argument("--uart-rx", default=None, help="WROOM RX GPIO connected to C6 TX")
    parser.add_argument("--uart-baud", default=None, help="UART baud rate")
    parser.add_argument("--http-port", default=None, help="HTTP server port")
    args = parser.parse_args()

    sdkconfig = Path("firmware/uplink-wroom32/sdkconfig")
    if not sdkconfig.exists():
        raise SystemExit("firmware/uplink-wroom32/sdkconfig not found. Run `idf.py set-target esp32` in firmware/uplink-wroom32 first.")

    lines = sdkconfig.read_text().splitlines(keepends=True)
    values = {
        CONFIG_KEYS["wifi_ssid"]: (args.wifi_ssid, True),
        CONFIG_KEYS["wifi_password"]: (args.wifi_password, True),
        CONFIG_KEYS["uart_tx"]: (args.uart_tx, False),
        CONFIG_KEYS["uart_rx"]: (args.uart_rx, False),
        CONFIG_KEYS["uart_baud"]: (args.uart_baud, False),
        CONFIG_KEYS["http_port"]: (args.http_port, False),
    }

    for key, (value, string_value) in values.items():
        if value is not None:
            lines = set_key(lines, key, value, string_value)

    sdkconfig.write_text("".join(lines))
    print("Updated firmware/uplink-wroom32/sdkconfig. Rebuild and flash the WROOM uplink.")


if __name__ == "__main__":
    main()
