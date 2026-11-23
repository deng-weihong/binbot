# Power Card

Standalone ESP32-C3 firmware using ESP-IDF v6.1. This project has no dependency
on BinBot, its component protocol, app, or server. The card joins Wi-Fi and
runs an IPv4 TCP command server on port **5000**. OTA downloads an application
binary from an **HTTP URL** supplied by a TCP or serial command.

## Current Behavior

- Boots into a serial console at 115200 baud, using UART0 by default.
- `status` reports firmware version, uptime, GPIOs, and the raw PW_LED input.
- `power` emits one 500 ms PWR_SW pulse; `reset` emits one 200 ms RESET_SW pulse.
- SW2 triggers one power pulse per press with a 50 ms debounce.
- Startup initializes both outputs inactive; a held SW2 must be released
  before it can trigger a new press. There is no automatic power-on action.
- GPIOs, output polarities, and 100-1000 ms pulse durations are configurable.
- Set any GPIO to `-1` to disable that signal.
- Wi-Fi credentials can be saved over serial; the card reconnects automatically.
- TCP supports `status`, `power`, `reset`, `ota`, and `ota_status`.
- A per-device token is generated once and saved in NVS. The serial `token`
  command displays it; each TCP request must include it.
- HTTP OTA runs in the background. Button pulses are blocked during an update;
  failed downloads leave the existing firmware selected.
- The default flash size is 4 MB with two 1700 KB OTA slots and boot rollback.

`power` simulates a physical PWR_SW press. It does not know whether the PC is
already running, so it can also request shutdown on a running PC. Power-state
sensing is exposed as a raw LED level; PC power state remains unknown until
the sense circuit and signal behavior are confirmed.

## Schematic Pin Map

| Signal | ESP32-C3 GPIO | Behavior |
| --- | --- | --- |
| IO-1 / Q1 / U3 / PW | 3 | High closes the PW contact; idle low |
| IO-2 / Q2 / U4 / RST | 4 | High closes the RST contact; idle low |
| IO-SW / SW2 | 5 | Low when pressed; internal pull-up enabled |
| PW_LED+ sense | 10 | Raw input; external 4.7k pull-down |
| U0TX / U0RX | 21 / 20 | UART0 console and download port |
| BOOT / EN | 9 / EN | ROM download and chip reset |

The provided schematic uses S8050 transistors to drive PhotoMOS input LEDs,
so both control GPIOs are active high. IO-1/IO-2/IO-SW are net names for
GPIO3/4/5; the latter two are also labeled MTMS/MTDI on the ESP32-C3 symbol.

The shown D4 has its anode at VDD-3V3 and its cathode at the GPIO10 sense
node. This can pull the sense input high even when PW_LED+ is inactive.
Verify that diode direction before using this signal as PC power feedback.
The firmware reports the raw level and does not infer on/off or sleep state.

## Layout

```text
CMakeLists.txt          Independent ESP-IDF project
build.sh                Linux toolchain setup and idf.py wrapper
sdkconfig.defaults      ESP32-C3 and schematic GPIO defaults
version.txt             Application version
main/app_main.c         Startup and serial console commands
main/board_io.c         PhotoMOS outputs, local button debounce, LED input
main/network.c          Wi-Fi, NVS configuration, reconnect, device token
main/protocol.c         TCP framing, JSON requests, HTTP URL validation
main/tcp_server.c       TCP listener and responses
main/ota_update.c       Background HTTP download, flash, validation, reboot
main/firmware_image.c   Firmware identity check before partition erase
main/Kconfig.projbuild  Hardware, Wi-Fi, and TCP configuration
tools/power_card_client.py  Python TCP client (standard library only)
tests/run.sh            Host tests with address/undefined/leak sanitizers
```

## Build

The default toolchain path is `/home/dwh/software/toolchains/esp-idf-v6.1`.
Set `IDF_PATH`, `IDF_TOOLS_PATH`, or `IDF_PYTHON_ENV_PATH` to use another
installation.

```sh
cd /home/dwh/project/deskscreen/power_card
bash build.sh
bash build.sh menuconfig
bash build.sh -p /dev/ttyACM1 flash monitor
```

Output: `build/power_card.bin`; generated configuration: `build/sdkconfig`.
Override the build directory using `POWER_CARD_BUILD_DIR` when needed.
First installation uses `flash`, which writes the bootloader, partition
table, initial OTA data, and application. The 0.1.0 serial-only build used a
different partition layout; installing this layout requires a full serial
flash. Future application upgrades use `power_card.bin` over HTTP OTA.

The build downloads the pinned `espressif/cjson` component on first use;
`dependencies.lock` records the dependency. No Windows toolchain is needed.

## First Network Setup

Use the 115200 baud serial console:

```text
wifi "Your SSID" "your-password"
```

This saves credentials in NVS and restarts the ESP32. For an open network,
use `wifi "Your SSID"`. Passwords must contain 8-63 bytes when nonempty;
SSIDs must contain 1-32 bytes. The card uses 2.4 GHz Wi-Fi and DHCP.
On the next boot:

```text
status
token
```

`status` shows the assigned IP and TCP port. `token` displays the 32-character
TCP token. Wi-Fi credentials and token survive OTA and normal restarts.
Alternatively configure Wi-Fi and the TCP port in `menuconfig > Power card`.
Saved serial credentials take precedence over build defaults. A blank SSID
leaves the serial console available for setup; there is no provisioning AP.

## TCP Protocol

Transport: IPv4 TCP, port 5000 by default, UTF-8 JSON, **one request per line**
ending with LF (`\n`); CRLF is also accepted. The server supports persistent
connections, up to four clients, and a maximum request size of 1024 bytes
excluding LF. Connections expire 120 seconds after the last complete request.
TCP fragmentation and several requests in one packet are supported.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | Optional string, max 64 bytes | Echoed in the response |
| `token` | Required string | Device token from the serial console |
| `cmd` | Required string | Command from the table below |
| `url` | String, max 512 bytes | Required only for `ota` |

Duplicate and unknown fields are rejected. Requests and responses contain
no literal newlines inside JSON; use standard JSON escaping for strings.

| Command | Result |
| --- | --- |
| `status` | Firmware version, uptime, IP/port, raw LED level, partition, OTA status |
| `power` | One configured PWR_SW pulse; response follows pulse completion |
| `reset` | One configured RESET_SW pulse; response follows pulse completion |
| `ota` | Accept an HTTP firmware URL and start the background update |
| `ota_status` | Update state, byte counts, percentage, target version, last error |

Example requests (replace `DEVICE_TOKEN`; each line ends with LF):

```json
{"id":"1","cmd":"status","token":"DEVICE_TOKEN"}
{"id":"2","cmd":"power","token":"DEVICE_TOKEN"}
{"id":"3","cmd":"reset","token":"DEVICE_TOKEN"}
{"id":"4","cmd":"ota","token":"DEVICE_TOKEN","url":"http://192.168.1.10:8000/power_card.bin"}
{"id":"5","cmd":"ota_status","token":"DEVICE_TOKEN"}
```

Example responses:

```json
{"id":"2","ok":true}
{"id":"4","ok":true,"data":{"accepted":true}}
{"id":"5","ok":true,"data":{"state":"downloading","busy":true,"received":262144,"total":1042304,"progress":25.1505,"version":"0.2.0","error":""}}
{"id":"6","ok":false,"error":"busy"}
```

Common errors: `unauthorized`, `invalid_json`, `invalid_request`,
`unknown_command`, `invalid_url`, `not_connected`, `busy`, `output_disabled`.
Frames containing NUL or exceeding the size limit return `invalid_frame`
and close the connection. Responses are newline-delimited JSON, too.

An `id` correlates a response with its request; it does not deduplicate commands.
Clients should not automatically retry `power` or `reset` after a lost response,
because the physical pulse may already have occurred. Use the TCP service and
plain HTTP firmware hosting within a trusted LAN; the token is not encryption.

## Command-Line Client

```sh
export POWER_CARD_TOKEN='token-from-serial-console'
python3 tools/power_card_client.py 192.168.1.50 status
python3 tools/power_card_client.py 192.168.1.50 power
python3 tools/power_card_client.py 192.168.1.50 reset
python3 tools/power_card_client.py 192.168.1.50 ota \
  --url http://192.168.1.10:8000/power_card.bin
python3 tools/power_card_client.py 192.168.1.50 ota_status
```

Use `--port` for a different listening port. The client sends one command per
connection and never retries a pulse automatically.

## HTTP OTA

Serve the build directory from a machine reachable by the card:

```sh
python3 -m http.server 8000 --bind 0.0.0.0 --directory build
```

Use that machine's LAN IP in the URL, not `127.0.0.1`. Send the `ota` TCP
command above, or run `ota http://192.168.1.10:8000/power_card.bin` over serial.
The URL must return HTTP 200 with `Content-Length`, without redirects, chunked
transfer, authentication, or compression. Only `http://` is supported here.
Download the application **power_card.bin**, not a bootloader or merged image.

Update states: `idle`, `queued`, `connecting`, `downloading`, `validating`,
`rebooting`, `failed`. Poll `ota_status` or serial `status` for progress.
100% downloaded does not mean validation has passed. On success the card
waits three seconds, then restarts the ESP32 and closes TCP connections.
Reconnect and use `status` to verify the running version/partition.
OTA state is kept in RAM and returns to `idle` after restart.

The downloader buffers the first 288 bytes across short reads and rejects
non-ESP32-C3 images, non-`power_card` projects, invalid descriptors, and images
without an appended digest before erasing the inactive slot. Downloads are
bounded by the slot size, a 10-second network I/O timeout, and a five-minute
overall deadline. A 4096-byte buffer streams the image to the inactive slot;
the whole image is never held in RAM. ESP-IDF validates the image checksum
and SHA-256 digest before selecting it for boot. Download/write/validation
failure keeps the old boot selection and releases the button lock.

Bootloader rollback is enabled. New firmware confirms its first boot after
GPIO initialization, network stack setup, TCP listener binding, and console
setup succeed. A reset or panic before confirmation allows the bootloader
to return to the previous valid slot. Internet/router availability is not
a confirmation condition. OTA never presses the motherboard's PW/RST contacts.

## Verification

After building:

```sh
bash tests/run.sh
```

Host tests compile the same framing, request parser, URL validator, and
firmware identity checker as the device. They cover fragmentation/coalescing,
authentication, invalid commands, duplicate fields, oversized frames, bounded
JSON nesting, and rejected firmware headers using the built application.
They do not emulate Wi-Fi, GPIO, flash writes, or bootloader rollback.

## Hardware Configuration

Defaults match the provided schematic. Adjust them in `menuconfig > Power card`
for another board. GPIO12-17 are reserved for flash; console pins and duplicate
GPIO assignments are rejected before any GPIO is configured. The PhotoMOS
contacts connect across the motherboard PW/RST headers; GPIO3/4 drive the
transistor inputs. R7/R9 keep the switches inactive during reset. Confirm
flash capacity before first flash.

Once wiring is confirmed, test `status`, `power`, `reset`, and SW2; then test
TCP control and one OTA cycle. This project has been compiled and host-tested
but has not been flashed or tested on hardware.
