# Binbot ESP32-C3 application project

This is one ESP-IDF project with build-time application selection. It keeps
the source tree shared and creates separate build/configuration outputs.

The four application profiles are:

- `round_screen`: small round screen, GC9A01A
- `holographic_screen`: holographic display entry, currently using the ILI9342C test transport
- `showcase_turntable`: showcase turntable, alternating left/right rotation
- `ambient_light`: WS2812 LEDs, GPIO19, four LEDs by default

On Linux, use ESP-IDF v6.1 and run:

```sh
bash build_variant.sh round_screen
bash build_variant.sh holographic_screen
bash build_variant.sh showcase_turntable
bash build_variant.sh ambient_light
# Build all four profiles
bash build_variant.sh all
```

The script uses `/home/dwh/software/toolchains/esp-idf-v6.1` and its
`espressif` subdirectory for the toolchain and Python environment. Set
`IDF_PATH` and `IDF_TOOLS_PATH` to use another installation.
Linux build outputs and generated sdkconfig files are kept in
`build/linux-uart/<profile>`.
New Linux builds start from the profile defaults, with UART OTA enabled and
Wi-Fi OTA disabled. `BINBOT_COMPONENT_BUILD_ROOT` overrides the output root.
Additional arguments are passed to `idf.py`, for example:

```sh
bash build_variant.sh round_screen -p /dev/ttyACM0 flash monitor
```

The profiles use separate build directories and configuration files, while
remaining one project:

- `build/linux-uart/round_screen` with the native Linux toolchain
- `build/linux-uart/holographic_screen` with the native Linux toolchain
- `build/linux-uart/showcase_turntable` with the native Linux toolchain
- `build/linux-uart/ambient_light` with the native Linux toolchain

The hardware pin map is defined in `components/board/include/board_pins.h`.
The display stack uses hardware SPI on GPIO6/GPIO7 at 40 MHz with DMA-backed
full-frame transfers. The graphics service owns the DMA-capable frame buffer.

The turntable profile initializes UART OTA but no SPI, display, Wi-Fi or file
system. It drives the H-bridge inputs on GPIO18 and GPIO19: left for 5
seconds, a 100 ms stop, right for 5 seconds, and repeats continuously.

The old names `gc9a01a` and `ili9342c` remain accepted as aliases for the
round-screen and holographic-screen profiles. The display defaults files
`sdkconfig.defaults.gc9a01a` and `sdkconfig.defaults.ili9342c` are retained.

## UART OTA and FATFS

All four profiles support UART1 OTA at fixed 460800 8N1, TX GPIO5 / RX GPIO1.
The BinBot connection uses `/dev/ttyS3`. Protocol code is shared with the
independent [binbot_component_protocol](../binbot_component_protocol/README.md)
C11 library through its ESP-IDF component adapter. The default library path
is `../binbot_component_protocol`; override `BINBOT_COMPONENT_PROTOCOL_DIR`
through `build_variant.sh` when the library is elsewhere:

```sh
bash build_variant.sh all \
  -D BINBOT_COMPONENT_PROTOCOL_DIR=/path/to/binbot_component_protocol build
```

Building this firmware does not require `binbot_app`. See
[component UART OTA](../binbot_app/docs/component-uart-ota.md) for wiring,
protocol fields, first installation, and tests.
Both ends start at 460800 without negotiation or fallback. UART event-driven
reception drains buffered bytes without a fixed-size read timeout.

Web display updates use the Server's PNG/JPEG/GIF normalization and UART file
commands to replace `display.gif`. ESP32 buffers packets in 4 KiB of RAM before
writing aligned blocks to FATFS. COMMIT flushes the final block, syncs and
validates the file before replacing the displayed asset. Transfer offsets are
held in RAM; after a reboot a replacement starts at zero.

Round display uploads use 240x240 GIFs; holographic uploads use 320x240 GIFs.
The holographic player flushes eight RGB565 rows at a time, keeping the decoded
indexed canvas in RAM without allocating a full RGB565 frame. Legacy 132x132
assets remain playable until replaced by a new upload.

The host decoder regression checks require Pillow, FFmpeg and a C compiler:

```sh
python3 tests/test_gif_rows.py
```

They compare decoded pixels with Pillow at all three resolutions, including
local palettes, all 256 colors, frame looping and row-buffer boundaries, with
AddressSanitizer and UndefinedBehaviorSanitizer enabled.

On BinBot, use `binbot_app --component status` and
`binbot_app --component ota /path/to/binbot_round_screen.bin` (or the matching
holographic/turntable binary). Each build embeds a distinct project name for
its profile to prevent cross-flashing. Existing echo-only or 115200
UART OTA images need a download-port flash of the updated firmware;
subsequent UART updates only change the
application and preserve storage. Turntables use a new dual-slot 2 MB layout
from `partitions_ota_turntable.csv`.

Ambient lights use the same dual-slot 2 MB layout without a storage partition.
The firmware identity is `binbot_ambient_light` (profile 5; profile 4 is reserved
for the slave MCU). Startup lights all four WS2812 pixels at RGB (255, 160, 80)
and 20% brightness. GPIO19 carries the GRB data via the ESP-IDF RMT driver, so
the console uses UART0 rather than native USB Serial/JTAG. First installation
flashes the bootloader, partition table, initial OTA data and application.
Subsequent updates use the existing UART OTA protocol with rollback support.

Light firmware v34 adds UART power, RGB color, brightness, effect speed and
timed shutdown controls. Solid, breathing, flowing hue, blink and heartbeat
effects run in a 25 ms task independently of the RV1106 page. Changing color or
brightness keeps the active deadline. Timer expiry switches the light off;
reboot restores the startup settings. See the light-control section in
[component UART OTA](../binbot_app/docs/component-uart-ota.md) for command fields
and CLI examples.

The two display profiles use the custom 4 MB flash layout in
`partitions_ota.csv`:

- `ota_0` and `ota_1`: two 1.0625 MiB application slots
- `storage`: 1.75 MiB FATFS partition at `0x240000`

The GIF assets are packaged into the FATFS image during the build and are
mounted at `/storage`. The display applications play the single
`display.gif` asset. The web upload path accepts an image with any user
filename and normalizes it to this runtime name.

## Legacy Wi-Fi OTA (Disabled by Default)

The network service and Python server below are retained for older firmware.
To build a display profile for Wi-Fi instead of UART, disable
`CONFIG_BINBOT_UART_OTA_ENABLE` and enable `CONFIG_BINBOT_OTA_ENABLE`.
Configure the Wi-Fi settings
with `idf.py menuconfig` or by editing the selected `sdkconfig.<profile>`:

```text
CONFIG_BINBOT_OTA_WIFI_SSID="your_wifi"
CONFIG_BINBOT_OTA_WIFI_PASSWORD="your_password"
CONFIG_BINBOT_OTA_URL="http://server/path/binbot_app_profiles.bin"
CONFIG_BINBOT_OTA_MANIFEST_URL="http://server/path/manifest.json"
CONFIG_BINBOT_OTA_TCP_ENABLE=y
CONFIG_BINBOT_OTA_TCP_HOST="192.168.100.211"
CONFIG_BINBOT_OTA_TCP_PORT=8001
```

`BINBOT_OTA_URL` and `BINBOT_OTA_MANIFEST_URL` remain as legacy configuration
aliases, but the active update path does not poll them. Use HTTPS and configure
certificate validation before deploying OTA on an untrusted network. The OTA
service runs only in the two display profiles;
the turntable profile does not link or initialize Wi-Fi, FATFS, or SPI.

The included Python server publishes a version manifest and serves the
application image. It reads the version from the ESP-IDF image automatically:

```sh
python tools/ota_server.py --firmware build/holographic_screen/binbot_app_profiles.bin --host 0.0.0.0 --port 8000
```

The device keeps a TCP control connection to the OTA server and sends
`HELLO <MAC> <version> <profile>`. The server does not cause an update just
because a new binary exists. Start the server with the matching firmware
profile so a broadcast cannot cross-flash the other display variant:

```sh
python tools/ota_server.py --firmware build/holographic_screen/binbot_app_profiles.bin \
  --host 0.0.0.0 --port 8000 --control-port 8001 \
  --profile holographic_screen \
  --public-base-url http://192.168.100.211:8000
```

Trigger an update explicitly through the HTTP control API:

```sh
curl -X POST http://127.0.0.1:8000/trigger \
  -H 'Content-Type: application/json' -d '{"version":"7"}'
```

The server sends `UPDATE <version> <firmware-url>` over the existing TCP
connection. The device then downloads the image over HTTP, checks that the
image version is newer, and reboots after a successful OTA. The TCP protocol
also uses `PING` and `PONG` keepalive messages. `/health`, `/devices`, and the
legacy `/manifest.json` endpoint remain available for diagnostics and
compatibility. The application displays an OTA progress bar only while an
image is actually downloading; otherwise it starts the normal GIF immediately.

The server can manage individual files in the mounted FATFS without replacing
the whole filesystem image or rebooting:

```sh
curl -X POST http://127.0.0.1:8000/trigger \
  -H 'Content-Type: application/json' -d '{"file":"display.gif"}'
```

That compatibility endpoint sends `FILE_UPDATE <name> <size> <sha256> <url>`.
The device streams the file with a small buffer, verifies its length and
SHA-256, and then replaces the old file. Playback is protected while the file
is being committed.

For general filesystem management, the server exposes single-file CRUD APIs.
The device must be selected with `device=<MAC>` when more than one matching
device is online. With one device and a matching server `--profile`, the
device parameter can be omitted:

```sh
# List files on the device
curl "http://127.0.0.1:8000/fs?device=9CCCC..."

# Create or overwrite one file; the request body is streamed to the device
curl -X PUT "http://127.0.0.1:8000/fs/display.gif?device=9CCCC..." \
  --data-binary @assets/display.gif -H 'Content-Type: application/octet-stream'

# Read one device file back to the computer
curl "http://127.0.0.1:8000/fs/read/display.gif?device=9CCCC..." -o downloaded-display.gif

# Delete one device file
curl -X DELETE "http://127.0.0.1:8000/fs/display.gif?device=9CCCC..."
```

`GET /fs`, `PUT /fs/<name>`, `GET /fs/read/<name>`, and
`DELETE /fs/<name>` map to `FS_LIST`, `FS_PUT`, `FS_GET`, and `FS_DELETE`
commands on the persistent TCP connection. File bytes use HTTP, not the
control channel. Writes are downloaded to a temporary file on the device,
verified, flushed, and atomically committed. Reads are streamed from the
device to the server. The server can be started for file management alone by
omitting `--firmware`:

```sh
python tools/ota_server.py --host 0.0.0.0 --port 8000 \
  --control-port 8001 --profile round_screen \
  --public-base-url http://192.168.100.211:8000
```

The first device setup must flash the bootloader, partition table, OTA data,
application, and FATFS image together. Use the Linux build wrapper, which
invokes the ESP-IDF v6.1 `idf.py` from the configured toolchain:

```sh
bash build_variant.sh round_screen -p /dev/ttyACM0 flash monitor
```

After the first setup, an OTA server can publish a new application binary or
manage individual storage files. Application OTA does not overwrite the FATFS
partition. The initial USB flash includes the writable `storage.bin`; later GIF
changes should use the file CRUD API above rather than replacing the partition.

For manual first flashing, use the generated `build/linux-uart/<profile>/flash_args`
file, which includes `storage.bin`. The board may need to enter download mode
by holding BOOT while pressing EN/RESET.
## Pixel screen asset tool

For a visual editor, run:

```sh
python -m pip install -r tools/requirements-pixel-screen.txt
python tools/pixel_screen_gui.py
```

The editor shows the source and pixel preview side by side and previews GIF
animation after generation.

The project includes an offline converter for photos and animated GIFs:

```sh
python -m pip install -r tools/requirements-pixel-screen.txt
python tools/pixel_screen.py input.jpg --output assets/photo.gif
```

The default output is a 240x240 round-screen asset with 4x4 pixel blocks,
32 colors, a black background, and a round mask. It also creates a PNG preview.
For an animated input GIF, up to 60 frames are converted and the frame delays
are preserved. The generated GIF is compatible with the current 240x240 GIF
player and can be included in `assets` before building the round-screen
storage image.

Useful options:

```sh
python tools/pixel_screen.py input.png --pixel-size 3 --colors 64
python tools/pixel_screen.py input.gif --output assets/photo.gif --max-frames 40
python tools/pixel_screen.py input.jpg --square --rgb565 build/photo.rgb565
```
