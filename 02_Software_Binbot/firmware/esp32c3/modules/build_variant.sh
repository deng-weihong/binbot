#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
profile="${1:-}"
case "$profile" in
    round_screen|holographic_screen|showcase_turntable|ambient_light) profiles=("$profile") ;;
    gc9a01a) profiles=(round_screen) ;;
    ili9342c) profiles=(holographic_screen) ;;
    all) profiles=(round_screen holographic_screen showcase_turntable ambient_light) ;;
    *)
        printf 'Usage: %s round_screen|holographic_screen|showcase_turntable|ambient_light|all [idf.py arguments]\n' "$0" >&2
        exit 2
        ;;
esac
shift
idf_args=("$@")
if (( ${#idf_args[@]} == 0 )); then
    idf_args=(build)
fi

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
for variant in "${profiles[@]}"; do
    build_dir="${BINBOT_COMPONENT_BUILD_ROOT:-$project_dir/build/linux-uart}/$variant"
    mkdir -p "$build_dir"
    # Keep native Linux-generated configuration separate from legacy build outputs.
    if [[ ! -f "$build_dir/sdkconfig" ]]; then
        cp "$project_dir/sdkconfig.defaults.$variant" "$build_dir/sdkconfig"
    fi
    python "$IDF_PATH/tools/idf.py" \
        -B "$build_dir" \
        -D "SDKCONFIG=$build_dir/sdkconfig" \
        -D "SDKCONFIG_DEFAULTS=$project_dir/sdkconfig.defaults.$variant" \
        -D IDF_TARGET=esp32c3 \
        "${idf_args[@]}"
done
