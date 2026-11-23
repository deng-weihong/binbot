# BinBot Web

React、TypeScript 和 Vite 实现的设备管理后台。登录后从设备列表进入单台设备的工作台，可查看首页、聊天记录、词本、基础控制、组件和设备设置。登录页也可进入示例预览。

## 开发与构建

需要 Node.js 22.12+。从 `web/` 执行：

```sh
npm ci
npm run dev
```

Vite 默认从 `http://127.0.0.1:8765` 代理 Server API。在本地 `.env` 中用 `VITE_BINBOT_API_TARGET` 指定其他后端地址，可参考 [.env.example](.env.example)。凭证从登录页面和服务器配置读取。

```sh
npm test
npm run build
```

部分测试会读取相邻 [App](../binbot_app/README.md) 的内置词卡和表盘清单。生产镜像通过 [Docker Compose](../deploy/compose.yaml) 构建，Nginx 为页面、API 和语音 WebSocket 提供同源入口。
