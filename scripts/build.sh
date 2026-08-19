#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
project_dir="${1:-.}"
project_path="${repo_root}/${project_dir}"

if [[ -z "${IDF_PATH:-}" || -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
  echo "ESP-IDF is not active. Source the ESP-IDF export/activate script first." >&2
  exit 2
fi

idf_py=("${IDF_PYTHON_ENV_PATH}/bin/python" "${IDF_PATH}/tools/idf.py")

cd "${project_path}"
export IDF_TARGET=esp32c6

"${idf_py[@]}" set-target esp32c6
"${idf_py[@]}" update-dependencies
"${idf_py[@]}" build
