# 共享协议

本目录是前端、Python 后端与设备 App 共同核对接口的入口。BBVP v1 语音规范与二进制向量已迁入；[MQTT v2](mqtt/v2/README.md) 已提供命令、状态和结果 schema，以及 Python / C 共用命令向量。HTTP OpenAPI 与生成前端类型仍是后续工作。

## 目标结构

```text
contracts/
├── http/
│   ├── openapi.json       后端模型导出的 HTTP 契约快照
│   └── examples/          成功、失败、冲突和异步命令示例
├── mqtt/v2/
│   ├── schemas/           状态、命令、结果、事件和同步通知
│   └── vectors/           Python / C 共用的有效与无效消息
└── voice/v1/              从现有后端迁入的 BBVP 规范与二进制向量
```

MQTT schema 从后端 Pydantic 模型导出并检查提交快照，公共向量验证 Python 与 C 实现。HTTP 目前保留原接口与手写前端适配类型；后续增加 OpenAPI 快照与类型生成检查。

## HTTP 约定

- 保留 `/api/v1/admin/session`、设备 `/state`、设备 `/commands`、命令查询及已有组件、摄像头、飞书和 Agent 接口。
- 新增设备资料、分领域设置、词本、学习事件与统一日程接口时，先定义输入、响应、错误码、分页和版本要求。
- 设备响应包含明确的设备归属；写操作使用幂等键或期望版本。接口成功接受与设备实际执行完成分开表示。
- 管理员继续使用现有会话；模型 Key、飞书 Secret、设备控制凭证只在对应服务端处理。

## MQTT 约定

Topic 前缀为 `binbot/v2/devices/{device_id}/`，后缀分为 `availability`、`state`、`command`、`result`、`event`、`sync`。协议使用 MQTT 3.1.1，schema 版本与 Broker 版本、语音版本独立。

命令至少包含版本、命令 ID、名称、参数、签发时间和过期时间；状态包含设备实例与领域版本。QoS 1 下命令去重，command 不 retain，PUBACK 不当作执行完成。

Broker 使用认证身份约束设备可访问的 Topic。设备不能通过消息中的任意设备 ID 控制其他设备，业务处理继续核对会话代次与消息归属。

音频、视频、图片、固件和整本词库继续使用各自的传输方式。MQTT 同步通知包含版本、受限下载信息和确认游标；单条控制消息有大小上限。

## 语音与设备兼容

语音 v1 保留现有规范和向量。App 测试默认读取本目录的 `voice/v1/`，可通过 `BINBOT_VOICE_CONTRACTS_DIR` 显式覆盖。

公共向量供 App 和 Server 测试使用。App 的组件 UART 协议实现位于 `app/third_party/binbot_component_protocol/`。
