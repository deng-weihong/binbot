# BinBot 软件与固件

## 源码

| 目录 | 内容 |
| --- | --- |
| [`binbot_app/`](binbot_app/README.md) | RV1106 设备端 C/C++、LVGL 界面与设备服务 |
| [`binbot_server/`](binbot_server/README.md) | Python API、语音服务、aMQTT 和 SQLite |
| [`binbot_web/`](binbot_web/README.md) | React 设备管理后台 |
| [`firmware/binbot_lcd_init/`](firmware/binbot_lcd_init/README.md) | PY32F002B 屏幕初始化固件与 Keil 工程 |
| [`contracts/`](contracts/README.md) | MQTT 控制协议、BBVP 语音协议和测试向量 |
| [`deploy/`](deploy/compose.yaml) | Web 和 Server 的 Docker Compose 示例 |

## 启动 Web 和 Server

从本目录复制配置示例并设置自己的管理员密码：

```sh
cp deploy/.env.example deploy/.env
chmod 600 deploy/.env
```

编辑 `deploy/.env` 后启动：

```sh
docker compose --env-file deploy/.env -f deploy/compose.yaml up --build -d
```

打开 `http://127.0.0.1:8080/`。默认使用 mock 语音和模型提供商。设备接入时，在自己的服务环境中登记设备 token，并在设备上填写连接参数。App 可先按 [模拟器说明](binbot_app/README.md) 在 Linux 上运行。

## 固件

| 设备 | 固件版本 | 使用说明 |
| --- | --- | --- |
| RV1106 主机 | App v53 | [系统固件](firmware/rv1106/README.md) |
| ESP32-C3 圆屏、全息屏、转盘、氛围灯 | v37 | [扩展模块固件](firmware/esp32c3/README.md) |

表中为 `20261106-rc1` 固件候选包。镜像和校验文件通过 GitHub Releases 分发。

第三方组件、字体和模型沿用随附许可证。项目源码的统一许可由项目维护者确定。
