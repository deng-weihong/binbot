# BinBot Component Protocol

Version 1.0.0. A portable C11 library for BinBot component UART communication
and the OTA receiver state machine. It depends only on the C standard library;
UART, flash, hashing, timers and application behavior belong to the consumers.

The wire protocol remains version 1, with fixed 460800 baud, 8N1 and no flow
control. Library releases and wire protocol versions are separate: moving the
library does not change the bytes sent between BinBot and ESP32.

## Layout

```text
include/binbot/component/protocol.h      Public wire protocol API
include/binbot/component/ota_receiver.h  Public OTA state machine API
include/binbot/component/file_receiver.h Public display asset state machine API
include/binbot/component/light.h         Light settings, timer and RGB renderer API
src/protocol.c                          Encoding, parsing, CRC and image metadata
src/ota_receiver.c                      Transfer validation and state transitions
src/file_receiver.c                     Display asset sessions and retry deduplication
src/light.c                             Light settings codec, effects and shutdown timer
tests/test_protocol.c                   Portable protocol and receiver tests
esp-idf/binbot_component_protocol/       ESP-IDF component adapter
```

The receiver calls `bb_component_ota_ops_t` callbacks for begin, write, finish,
abort and activate. The finish callback verifies SHA-256 and the full firmware
image; the activate callback selects the boot image and schedules the reboot.
The receiver itself owns transfer IDs, offsets, retry deduplication and state.
Each receiver and parser instance must be accessed serially by its owner.

The display file receiver uses the same 512-byte chunks and offset ACKs.
A new `FILE_BEGIN` returns offset zero, including after a committed or aborted
file. Repeating BEGIN for the active transfer ID returns that session's current
offset; the storage callback is not reopened. Tests check ACK offsets while
alternating different file sizes and restarting after an abort.

## Standalone Build and Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

No BinBot application checkout, ESP-IDF, OpenSSL or serial hardware is required.
Set `BUILD_TESTING=OFF` when only the static library is needed.

## CMake Consumer

```cmake
add_subdirectory("${BINBOT_COMPONENT_PROTOCOL_DIR}"
                 "${CMAKE_CURRENT_BINARY_DIR}/component_protocol")
target_link_libraries(your_target PRIVATE binbot_component_protocol)
```

The target exports its include directory and C11 requirement. Existing public
includes remain `binbot/component/protocol.h` and `binbot/component/ota_receiver.h`.

## ESP-IDF Consumer

Before including ESP-IDF's `project.cmake`, add the adapter directory:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
     "${BINBOT_COMPONENT_PROTOCOL_DIR}/esp-idf/binbot_component_protocol")
```

Declare `PRIV_REQUIRES binbot_component_protocol` in the component that uses
the protocol, or `REQUIRES` if its public headers expose protocol types.
The adapter builds the same sources as the native CMake target.

## BinBot Integration

Both `binbot_app` and `binbot_component` default to a sibling directory named
`binbot_component_protocol`. Each accepts a CMake cache path override:

```sh
cmake -S binbot_app -B binbot_app/build/component-host \
  -DBINBOT_COMPONENT_PROTOCOL_DIR=/path/to/binbot_component_protocol
cd binbot_component
bash build_variant.sh all \
  -D BINBOT_COMPONENT_PROTOCOL_DIR=/path/to/binbot_component_protocol build
```

Each application can build with this library without the other application.
The Linux client owns termios, downloads and host-side OTA orchestration; the
ESP32 service owns its UART driver, flash writes and boot confirmation.
Select a known library revision for release builds and check wire compatibility
when updating that dependency.

Components send an `ANNOUNCE` event (`type=3`, `command=0x0003`) at startup
and again when initialization becomes healthy. Its payload contains the
128-byte HELLO body followed by motor direction and speed (zero for profiles
without a motor). The host acknowledges the sequence with a response containing
`status:u16=0` and the matching `boot_id:u64`. Unacknowledged events retry every
250 ms. The portable `bb_component_announcement_*` functions track retries and
validate acknowledgements; a successful HELLO or GET_STATUS also confirms the
reported health state for compatibility with older hosts.

Ambient-light profile 5 advertises capability bit `0x02`. `LIGHT_GET` (0x0070)
and `LIGHT_SET` (0x0071) read and update power, one of five effects, RGB color,
brightness, speed and shutdown minutes. The portable light model uses a
monotonic millisecond clock and emits scaled RGB pixels; its consumer owns the
LED transport and task scheduling. Timer deadlines survive setting changes
until the timer selection changes or the light is switched off. Settings and
remaining time are queried separately, retaining the existing HELLO and
ANNOUNCE sizes and wire version.
