# BinBot App

RV1106 桌面设备的 C/C++ 与 LVGL 程序，包含界面、音频 IPC、语音 WebSocket、MQTT 控制、组件通信和设备设置。当前源码版本见 [version.txt](version.txt)。

## 在 Linux 上运行模拟器

需要 CMake 3.20+、C/C++ 编译器、pkg-config，以及 SDL2、FreeType、zlib、cJSON 和 D-Bus 的开发包。以 Debian 或 Ubuntu 为例：

```sh
sudo apt install build-essential cmake pkg-config libsdl2-dev libfreetype-dev zlib1g-dev libcjson-dev libdbus-1-dev
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --test-dir build/host-simulator --output-on-failure
BINBOT_ASSET_ROOT="$PWD/assets" BINBOT_STATE_PATH=/tmp/binbot-simulator.ini build/host-simulator/bin/binbot_app
```

## 构建 RV1106 程序

准备 Rockchip ARM/uClibc 工具链和已构建的 RV1106 SDK。SDK 的 Buildroot sysroot 需提供 FreeType、zlib、cJSON、D-Bus、ALSA、Opus、libwebsockets 和 OpenSSL。RK MPI、RGA、RKNN、RockIVA 和音频处理库由 SDK 提供。按实际安装位置填写以下路径：

```sh
export RV1106_TOOLCHAIN_ROOT=/path/to/arm-rockchip830-linux-uclibcgnueabihf
export RV1106_BUILDROOT_ROOT=/path/to/buildroot-2023.02.6
cmake --preset rv1106-release -DRV1106_SDK_ROOT=/path/to/rv1106_deskscreen
cmake --build --preset rv1106-release --parallel
```

MQTT 控制的构建与接入见 [binbot_control](services/binbot_control/README.md)。语音与控制共用设备 ID 和设备 token，对应协议见 [共享协议](../contracts/README.md)。

设备使用由 root 管理的 `/oem/binbot.env`。按 [配置示例](scripts/device/binbot.env.example) 填入自己的服务器地址和设备凭证，文件权限设为 0600。编译产物默认在 `build/rv1106/bin/`，界面资源在 `assets/`，固件资料见 [RV1106 系统固件](../firmware/rv1106/README.md)。

唤醒模型及 SDK 依赖由设备开发环境提供。开启 `BINBOT_BUILD_WAKE` 时需设置 `BINBOT_MIYA_NATIVE_DIR`；Snowboy 和 Hi Xiaowen 分别使用 `BINBOT_SNOWBOY_ROOT` 和 `NCNN_SOURCE_DIR` 指定各自的依赖。开启视觉功能时，将所用模型放在 `/oem/assets/vision/` 下对应目录。发音资源可通过 [Server 脚本](../binbot_server/README.md) 生成后按设备需要安装。
