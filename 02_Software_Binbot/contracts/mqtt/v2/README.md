# 设备控制 v2

MQTT 3.1.1、QoS 1，控制独立于 BBVP 语音 v1。基础命令为 `screen.brightness.set`（1–100）和 `audio.volume.set`（0–100）；新 App 通过 `workspace_v1` 扩展表盘、手势、麦克风、白噪音、专注、待办、组件和学习同步，通过 `camera_publish_v2` 执行摄像头租约。JSON schema 位于 `schemas/`，Python 与 C 共用命令向量位于 `vectors/commands.json`。

## 身份与 Topic

用户名是设备 ID，密码是该设备随机 token；client ID 为 `bb-{device_id}:{connection_nonce}`。必须显式配置 token，语音开发模式的匿名开关不能绕过 MQTT 认证。

所有 Topic 都以 `binbot/v2/devices/{device_id}/` 开头，设备只能访问自己的完整 Topic，通配符与跨设备访问拒绝：

| 后缀 | 方向 | 当前行为 |
| --- | --- | --- |
| `command` | 后端 → 设备 | 即时控制，不 retain、不离线重放 |
| `state` | 设备 → 后端 | App 实例、版本、已确认设置、能力、ready；不 retain |
| `result` | 设备 → 后端 | 实际执行与保存结果；不 retain |
| `availability` | 设备 → 后端 | 遗嘱预留；不作为业务在线的唯一依据 |
| `event`、`sync` | 上行、下行 | ACL 预留，业务尚未实现 |

后端拒绝 retained 状态和回执；单条业务消息最多 16 KiB。网页通过 HTTP 操作，不能直接发布 MQTT 命令。

## 命令与状态

命令严格包含 `type`、`schema_version`、`boot_id`、`command_id`、`name`、`payload`、`issued_at`、`expires_at`。时间为 Unix 秒，TTL 1–60 秒，App 容忍最多 5 秒的提前签发偏差；设备时钟错误时返回失败。命令绑定当前 App 的 32 位十六进制 `boot_id`。

状态由 App 每次变更和每 5 秒生成，独立进程转发并最多缓存 10 秒。`revision` 在同一个 App 实例内递增，后端忽略低版本与已淘汰实例。未确认的声卡音量或背光值为 `null`。MQTT 连接、App ready、语音在线通过 HTTP 分别返回为 `control_online`、`control_ready`、`voice_online`。

命令先保存、后发送。PUBACK 只能说明 MQTT 送达；只有当前设备、当前连接、当前 App 实例针对待执行命令的 `result` 才能结束命令。断线、超时、服务重启均将未决命令标为失败并说明结果未确认，不自动重放。HTTP 幂等键按设备隔离，同键不同参数返回 409。

App 去重日志保存最近 128 条操作，执行前 fsync；有效期内不淘汰记录。音量等待声卡回执，背光等待驱动设置和配置保存。结果落盘失败、声卡失败或背光失败不会报告成功；重启前未确认的命令不会再次执行。

## 工作台扩展

`state` 增加 `features`、`system` 与最多 8 条本批学习事件。学习记录先在设备 fsync，后台事务提交并去重后发送 `learning.events.ack`；控制命令仍不离线排队。设备词卡最多 30 词，按到期和新词分批，空批次表示等待到期复习。

学习事件可带 `grade`（`known`、`fuzzy`、`forgot`）和 `retry`（本批重测）。事件沿用编号、词本、词和实际作答时间，旧版 `remembered` 布尔字段继续兼容。后台用首次有效反馈安排跨天复习，重测更新短时计划。网页通过 `/workspace/learning` 获取当前词本的同一队列。

设备上报 `features.vocabulary.studyPlanVersion=1` 后，`learning.deck.set` 带 `plan`。其中 `due` 和 `new` 是今日待复习、待学词数，`learned_today` 是今日已学新词数，`next_review_at` 是未来最近的到期时间（Unix 秒，暂无为 0），`new_mask` 标记本批的新词。`studying` 表示设备正在显示本批词卡、完成页，或已保存了尚未答完的批次。完成本批并离开页面后，设备可接收同一词本的下一批。

麦克风和白噪音等待音频进程回执，组件设置等待外设结果，屏幕换图等待传输完成。图库文件仍通过 HTTP 下载；MQTT 只传任务与回执。工作台合并本地和飞书日历，防止旧语音快照覆盖本地日程。

系统信息每 10 秒刷新；报告超过预算时缩短可选的已知词列表和图片索引，并保留真实词数与待确认学习事件。完整图库由服务器设备索引保存。

## 生成与验证

在 `backend/` 执行：

```sh
.venv/bin/python scripts/export-control-contracts.py
.venv/bin/python scripts/export-control-contracts.py --check
.venv/bin/python -m pytest tests/test_control_contracts.py
```

JSON schema 描述字段约束；时间差、当前实例、去重、认证、ACL 与设备实际执行由运行时代码校验。C 测试将向量的固定时刻和实例替换成当前测试时刻/实例后执行，保留缺失字段与无效参数。
