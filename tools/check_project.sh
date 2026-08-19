#!/usr/bin/env bash
set -euo pipefail

required=(
  CMakeLists.txt
  main/probe_main.c
  components/probe_core/CMakeLists.txt
  components/probe_core/idf_component.yml
  components/probe_core/src/probe_thread.c
  components/probe_core/src/probe_bridge.c
  components/probe_matter/src/probe_matter.cpp
  firmware/uplink-wroom32/main/uplink_main.c
  docs/flashing.md
  .github/workflows/build.yml
)

for path in "${required[@]}"; do
  test -f "${path}" || {
    echo "missing ${path}" >&2
    exit 1
  }
done

echo "project skeleton ok"
