# BinBot RV1106 系统固件

首批提供集成 BinBot App 的预编译系统固件。应用和系统开发资料将继续整理，
后续分批公开。固件通过项目的 GitHub Releases 分发，大文件不提交到 Git 历史。

当前整理版本：`binbot-rv1106-app53-20261106-rc1`。
这是待实机验收的候选版本，已完成编译及离线镜像检查。

## 适用设备

- 当前 BinBot DeskScreen RV1106 主板，eMMC 至少 8 GB。
- 720 × 720 显示屏、GC2053 摄像头、AIC8800D80 Wi-Fi/蓝牙模块。
- Linux 5.10.160，BinBot App 版本 53，默认 Miya 本地唤醒。

固件包含主界面、音频、唤醒、语音连接、从机通信、扩展模块通信、相机和
设备 Agent 程序，以及配套资源和运行库。开机由 BinBot 服务管理，保留底层
驱动初始化。CH582 与 ESP32 模块的固件不由本刷机包更新。

## 固件包

压缩包内包含：

```text
update.img                  RV1106 eMMC 统一刷机镜像
download.bin                匹配的 Rockchip USB 下载加载器
README.md                   刷机及首次使用说明
SHA256SUMS                  包内文件的 SHA256 校验值
binbot-release.json         App 版本、程序和运行库校验值
binbot-verification.json    离线镜像检查结果
binbot.env.example          服务器和设备配置示例
licenses/                   第三方许可文件
```

## 刷机

1. 下载固件包，校验其 SHA256，解压后再次校验包内文件。
2. 备份已有设备的配置和数据。完整镜像包含 `userdata`，整机刷写会覆盖原数据。
3. 按主板的下载/Maskrom 流程连接 USB，使用 Rockchip RKDevTool 的固件升级
   功能选择 `update.img` 并写入 eMMC。
4. Linux 下也可以使用 Rockchip `upgrade_tool` 写入统一镜像：

```sh
sha256sum -c SHA256SUMS
sudo upgrade_tool uf update.img
```

写入成功后重新启动，检查屏幕、触控、旋钮、音频、相机、蓝牙及网络。
离线检查不能替代本机实测，候选版本完成实机验收后再标记为正式版本。

## 登录与服务器配置

系统登录用户名为 `root`，初始密码为 `binbot`。
首次启动可通过设备界面配置 Wi-Fi，设备 IP 可从串口或路由器查看。
固件未配置开发环境的 Wi-Fi、设备令牌或模型服务密钥。

登录设备后编辑：

```sh
vi /oem/binbot.env
```

填写自建服务器地址、设备编号，以及服务端签发的设备令牌：

```sh
BINBOT_DIALOG_URL='wss://your-server/v1/device/ws'
BINBOT_DEVICE_ID='your-device-id'
BINBOT_DEVICE_TOKEN='your-device-token'
```

文件中的默认地址仅为初始连接配置，需要与实际服务器和设备登记一致。
未配置服务器时，本地界面和本地功能仍可使用，云端功能需要有效的服务连接。
修改后执行：

```sh
chmod 600 /oem/binbot.env
/etc/init.d/S99binbot restart
/etc/init.d/S99binbot status
```

设备 Agent 的模型密钥和相机云端推流地址也从此文件配置。
应用日志位于 `/tmp/binbot_*.log`，语音连接问题可先查看
`/tmp/binbot_dialog.log`。固件信息位于 `/oem/binbot-release.json`。

## 第三方组件

系统包含 Linux、U-Boot、Buildroot 生成的用户空间，以及第三方运行库、字体和
模型。它们分别适用各自许可证；固件包和设备 `/oem/licenses` 提供部分许可文本，
资源目录保留相应模型、字体许可。厂商 SDK 组件按其授权处理。
