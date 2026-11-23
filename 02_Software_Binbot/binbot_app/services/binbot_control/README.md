# 独立 MQTT 控制进程

`binbot_control` 使用 libmosquitto 作为 MQTT 3.1.1 客户端，连接 Python 后端管理的 aMQTT。它通过独立 Unix `SOCK_SEQPACKET` socket 与 App 交换控制消息，不经过语音连接或音频命令转发。已接通背光、音量、表盘、手势、麦克风、白噪音、专注、待办、组件、动态词卡与摄像头租约；语音保持独立。


## 构建

在 `app/` 下先运行 `python3 scripts/build-control-deps.py rv1106`，再配置完整工程：

```sh
cmake -S . -B build/rv1106 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1106.cmake \
  -DBINBOT_BUILD_CONTROL=ON \
  -DMOSQUITTO_INCLUDE_DIR="$PWD/build/platform-deps/rv1106/include" \
  -DMOSQUITTO_LIBRARY="$PWD/build/platform-deps/rv1106/lib/libmosquitto_static.a"
cmake --build build/rv1106 -j2 --target binbot_app binbot_audio binbot_control
```

仅本机控制进程和 IPC 测试可以独立构建；需要本机 cJSON、OpenSSL 与 pkg-config：

```sh
python3 scripts/build-control-deps.py host
cmake -S services/binbot_control -B build/control-host \
  -DCMAKE_PREFIX_PATH="$PWD/build/platform-deps/host"
cmake --build build/control-host -j2
ctest --test-dir build/control-host --output-on-failure
```

共享 MQTT 测试向量默认来自 `../contracts/mqtt/v2/`，可通过 `BINBOT_CONTROL_CONTRACTS_DIR` CMake 参数指定。语音测试同样支持 `BINBOT_VOICE_CONTRACTS_DIR`。

## 设备配置

在设备 root 管理的 `/oem/binbot.env` 配置以下字段。每台设备使用不同 ID、独立随机 token，后端 `BINBOT_DEVICE_TOKENS_JSON` 中维护对应关系；语音进程继续使用相同设备身份。

```sh
BINBOT_CONTROL_ENABLE=1
BINBOT_DEVICE_ID=desk-unique-id
BINBOT_DEVICE_TOKEN=replace-with-a-random-device-token
BINBOT_MQTT_HOST=your-server-host
BINBOT_MQTT_PORT=1883
# TLS listener: also configure its trusted CA file.
# BINBOT_MQTT_CA_FILE=/oem/ssl/certs/your-mqtt-ca.crt
# BINBOT_CONTROL_SOCKET=/tmp/binbot_control.sock
# BINBOT_CONTROL_JOURNAL=/oem/data/binbot_state.ini.control.json
```


将编译好的 `binbot_control` 安装到设备的 `/oem/` 后，通过 [App 配置示例](../../scripts/device/binbot.env.example) 设置连接信息。
