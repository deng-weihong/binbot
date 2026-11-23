# BinBot Server

Python 后端提供 aiohttp API、aMQTT 设备控制、BBVP 语音 WebSocket、设备工作台和 SQLite 持久化。每台设备的聊天记录在该设备工作台自动同步。

## 本地开发

需要 Python 3.11+、libopus 和 FFmpeg。以下命令从 `server/` 执行：

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -e '.[dev,feishu,mcp]'

export BINBOT_ADMIN_USERNAME=admin
read -rs -p '设置本地管理员密码：' BINBOT_ADMIN_PASSWORD
export BINBOT_ADMIN_PASSWORD
printf '\n'
binbot-server --config config.dev.toml
```

开发配置绑定回环地址，HTTP 与语音使用 8765，MQTT 使用 1885。mock 提供商可用于本地联调，运行数据写入 `runtime/`。部署示例见 [Docker Compose](../deploy/compose.yaml)。

连接设备时，在服务环境中用 `BINBOT_DEVICE_TOKENS_JSON` 登记每台设备的 ID 和独立 token。实际模型和语音服务在 Web「公共服务设置」里配置，飞书在各设备设置内授权。

## 验证

```sh
python -m pytest
python -m pytest -m integration
python scripts/export-control-contracts.py --check
```

协议和测试向量见 [contracts](../contracts/README.md)。装入 `.[audio]` 可使用 `scripts/generate-cet4-audio.py` 生成词卡发音，输出位于 `runtime/cet4-audio/`。
