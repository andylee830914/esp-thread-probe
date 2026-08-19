# JSON API

All responses are `application/json`. The ESP-WROOM-32 serves the HTTP API and
forwards probe endpoints to the ESP32-C6 over UART.

## `GET /uplink`

Returns WROOM-only status and does not query the C6.

## `GET /health`

Returns uplink/probe health. If the WROOM is alive but the C6 does not answer
over UART, the endpoint returns a probe timeout response.

## `GET /info`

Returns firmware identity, uptime, chip details, feature flags, and compact Thread state.

## `GET /mesh`

Returns a combined mesh snapshot for dashboards and Home Assistant sensors.

## `GET /neighbors`

Returns OpenThread neighbor entries with RLOC16, child flag, age, RSSI, and link quality.

## `GET /routers`

Returns the Thread router table, including router id, RLOC16, next hop, path
cost, link quality, and active-router state when OpenThread exposes it.

## `GET /children`

Returns children attached to this probe when the probe is currently a parent.

## `GET /topology`

Returns a combined topology snapshot with this probe, leader data, routers,
neighbors, and local children.

## `GET /router-neighbors/scan`

Starts a Thread Network Diagnostic scan against known Thread routers. The scan
uses OpenThread TMF Network Diagnostic client support and completes
asynchronously.

## `GET /router-neighbors`

Returns the last cached router-neighbor diagnostic result, including router
links and child MAC addresses collected by the background scanner.

## `GET /router`

Returns the current Thread role and router/parent information.

## `GET /ipaddr`

Returns Thread IPv6 unicast addresses.

## `GET /leader`

Returns leader data including partition id and data versions.

## `GET /dataset`

Returns a safe summary of the active operational dataset. The network key is not exposed.
