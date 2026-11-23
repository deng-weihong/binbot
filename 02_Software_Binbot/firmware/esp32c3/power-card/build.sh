#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
    printf 'Set IDF_PATH to your ESP-IDF v6.1 installation.\n' >&2
    exit 1
fi
if [[ -z "${IDF_TOOLS_PATH:-}" && -d "$IDF_PATH/espressif" ]]; then
    export IDF_TOOLS_PATH="$IDF_PATH/espressif"
fi
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" && -n "${IDF_TOOLS_PATH:-}" ]]; then
    for python_env in "$IDF_TOOLS_PATH"/python_env/idf6.1_py*_env; do
        if [[ -x "$python_env/bin/python" && -x "$python_env/bin/pip" ]]; then
            export IDF_PYTHON_ENV_PATH="$python_env"
            export PATH="$python_env/bin:$PATH"
            break
        fi
    done
fi
source "$IDF_PATH/export.sh"

cd "$project_dir"
build_dir="${POWER_CARD_BUILD_DIR:-$project_dir/build}"
if [[ "$build_dir" != /* ]]; then
    build_dir="$project_dir/$build_dir"
fi
if (( $# == 0 )); then
    set -- build
fi
python "$IDF_PATH/tools/idf.py" \
    -B "$build_dir" \
    -D "SDKCONFIG=$build_dir/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$project_dir/sdkconfig.defaults" \
    -D IDF_TARGET=esp32c3 \
    "$@"
