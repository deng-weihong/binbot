#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
idf_dir="${IDF_PATH:?Set IDF_PATH to your ESP-IDF v6.1 installation}"
test_build_dir="${POWER_CARD_BUILD_DIR:-$project_dir/build}"
if [[ "$test_build_dir" != /* ]]; then
    test_build_dir="$project_dir/$test_build_dir"
fi
cjson_dir="$project_dir/managed_components/espressif__cjson/cJSON"
cc -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCJSON_NESTING_LIMIT=8 \
    -I "$project_dir/main" -I "$cjson_dir" -I "$test_build_dir/config" \
    -I "$idf_dir/components/http_parser" \
    -I "$idf_dir/components/esp_common/include" \
    -I "$idf_dir/components/esp_app_format/include" \
    -I "$idf_dir/components/bootloader_support/include" \
    "$project_dir/tests/test_protocol.c" "$project_dir/main/protocol.c" \
    "$project_dir/main/firmware_image.c" "$cjson_dir/cJSON.c" \
    "$idf_dir/components/http_parser/http_parser.c" -lm \
    -o "$test_build_dir/test_protocol"
"$test_build_dir/test_protocol" "$test_build_dir/power_card.bin"
