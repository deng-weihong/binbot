# BinBot ESP32 扩展模块固件

本批提供四种 ESP32-C3 扩展模块的预编译固件。每种型号分别发布一个压缩包，
当前版本为 v37，发布标记为 `20261106-rc1`。已完成编译和离线检查，尚未进行
本批固件的实机烧录验收。

| 型号 | 硬件 | Flash 布局 | 固件身份 |
| --- | --- | --- | --- |
| `round_screen` | GC9A01A，240 × 240 圆屏/笔筒模块 | 4 MB，含显示资源分区 | `binbot_round_screen` |
| `holographic_screen` | ILI9342C，320 × 240 全息显示模块 | 4 MB，含显示资源分区 | `binbot_holographic_screen` |
| `showcase_turntable` | H 桥电机转盘 | 2 MB | `binbot_showcase_turntable` |
| `ambient_light` | GPIO19，默认 4 颗 WS2812 | 2 MB | `binbot_ambient_light` |

两种显示模块各有两个 `0x110000` 字节应用槽；转盘和氛围灯各有两个
`0x0e0000` 字节应用槽。各型号使用不同固件身份，按实际硬件选择，后续 OTA
也必须保持型号一致。

## 固件包

每个型号的压缩包都包含以下文件：

```text
factory.bin                 首次烧录的完整合并镜像，写入地址 0x0
ota.bin                     后续 UART OTA 使用的应用固件
README.md                   当前型号的烧录和使用说明
release.json                型号、版本、容量、分区和文件校验信息
verification.json           离线检查结果
SHA256SUMS                  包内文件的 SHA256 校验值
licenses/                   随包第三方许可文件
```

完整镜像含 bootloader、分区表、初始 OTA 数据和应用；两种显示模块还包含
`storage.bin` 中的初始显示资源。镜像从构建系统提供的烧录地址合并生成。
完整烧录会清除该布局范围内的原配置和显示资源；日常升级使用 `ota.bin`。

固件大文件通过 GitHub Releases 分发，本目录的 `firmware/` 仅存放本地待发布
压缩包，不提交到 Git 历史。源码将在后续整理。

## 首次烧录

安装 Python 和 esptool 5.x，校验并解压对应型号的压缩包。连接模块下载口，
按模块硬件流程进入下载模式，执行：

```sh
python -m pip install 'esptool>=5,<6'
sha256sum -c SHA256SUMS
python -m esptool --chip esp32c3 --port PORT --baud 460800 \
  write-flash 0x0 factory.bin
```

将 `PORT` 替换为实际端口，例如 Windows 的 `COM5` 或 Linux 的 `/dev/ttyACM0`。
使用 Espressif 图形烧录工具时，选择 ESP32-C3、DIO、80 MHz、该型号的 Flash
容量，并将 `factory.bin` 写入 `0x0`。烧录后重新上电。

转盘上电后会自动左右交替运行；氛围灯上电默认显示 20% 亮度的暖白色。
转盘和灯使用 GPIO18/19 等原生 USB 引脚，应用运行后应使用 UART0 查看日志；
再次进入芯片下载模式后仍可重新烧录。

## 连接 BinBot 与后续升级

四种模块使用 UART1，固定 460800 baud、8N1、无流控：

| ESP32-C3 引脚 | 连接 RV1106 |
| --- | --- |
| GPIO1 / RX | UART3 TX |
| GPIO5 / TX | UART3 RX |
| GND | GND |

使用 3.3 V 逻辑电平。RV1106 设备节点为 `/dev/ttyS3`，UART4 用于 CH582 从机。
v1 协议一次连接一个 ESP32 模块。本批模块固件使用 UART OTA，未开启 Wi-Fi OTA，
不需要配置服务器地址、Wi-Fi 或设备令牌。

在已安装配套 BinBot App 的主机上，可通过 Console 上传对应的 `ota.bin` 并升级。
也可将应用固件传到 RV1106 后使用独立工具，操作前先停止占用串口的界面：

```sh
killall binbot_app
/oem/binbot_component --uart /dev/ttyS3 status
/oem/binbot_component --uart /dev/ttyS3 ota /tmp/ota.bin
/etc/init.d/S99binbot restart
```

`ota.bin` 只更新应用，保留分区表、NVS 和显示资源。旧的 115200 波特率版本、
仅回显程序或不同分区表，需先通过下载口安装完整镜像。完整镜像不能作为 OTA
应用固件上传。UART 协议版本为 1，目标配套 RV1106 App v53。

显示模块的图片可在后续通过主机上传：圆屏为 240 × 240 GIF，全息屏为
320 × 240 GIF，模块运行时使用统一的 `display.gif`。
