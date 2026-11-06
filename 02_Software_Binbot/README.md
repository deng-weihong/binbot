# BinBot 软件与固件

当前先整理编译好的固件，源码与构建资料后续分批公开。各固件包分别提供使用说明、
版本信息和 SHA256 校验，二进制大文件通过 GitHub Releases 分发。

| 设备 | 当前版本 | 使用说明 |
| --- | --- | --- |
| RV1106 主机，已集成 App | App v53 | [系统固件](binbot_sdk/README.md) |
| ESP32-C3 圆屏/笔筒、全息屏、转盘、氛围灯 | v37 | [扩展模块固件](binbot_component/README.md) |

本批均为 `20261106-rc1` 候选包，已完成编译与离线检查，尚未完成本批全部硬件
的实机验收。各包按设备和型号使用；ESP32 首次烧录使用 `factory.bin`，后续 OTA
使用 `ota.bin`，RV1106 则使用其专用的 `update.img`。
