#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
idf.py -p "${PORT}" flash monitor
