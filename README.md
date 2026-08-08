# 元宝 Bot 统一服务器

> 腾讯元宝开放平台 Bot 的一体化管理服务器 —— **纯 C++ 后端 + 现代 Web 前端**，无需 Python。

一套开箱即用的元宝 Bot 管理控制台：浏览器打开即可连接、收发消息、刷屏、发贴纸/图片、管理群成员，并内置 **Apple Liquid Glass 液态玻璃** 视觉引擎与多线程并发处理。

---

## ✨ 功能特性

### 🤖 Bot 核心
- **WebSocket 长连接**：鉴权（auth-bind）、心跳（ping/pong）、断线自动重连
- **消息收发**：群消息、私聊（DM）、@成员、@全体、引用回复
- **实时消息流**：群消息实时推送到前端（缓存 + 日志历史合并展示）
- **贴纸库**：内置 80+ 腾讯表情贴纸，支持纯贴纸 / 贴纸+文字 / 贴纸+@
- **图片/文件发送**：支持 URL 图片与本地文件上传发送

### ⚡ 增强能力
- **刷屏引擎**：15 种文本变换（全角 / MOCK / 乱码 / 彩虹 / 地震 / 矩阵等），自定义次数与间隔
- **大模型回复**：接入 OpenAI 兼容 API（DeepSeek / 通义千问 / 智谱等），群消息智能回复
- **转发模式**：@元宝 自动转发到目标群、队列管理
- **群成员管理**：实时获取群成员列表、群主标记、昵称备注
- **消息日志**：JSONL + 文本双格式落盘，历史消息回放、下载、清空

### 🧊 液态玻璃（Liquid Glass）
- Apple 风格液态玻璃视觉：真实折射 + RGB 三通道色散 + 动态光影
- **物理引擎**（弹簧-阻尼）+ 设备感应器（重力/陀螺仪）+ 鼠标视差
- **GLSL 着色器引擎**（`glass.h`）与 **C++ 高性能组件**（`class.h`，位移贴图 / 边缘环遮罩生成）
- 10 套玻璃预设（Neon / Crystal / Aurora / Matrix / Holographic 等）
- 性能极致 / 性能兼容模式互斥

### 🚀 工程能力
- **纯 C++17 实现**，无 Python 运行时依赖
- **多线程架构**：单 acceptor + 每连接一线程，阻塞请求互不影响
- SSE 事件推送、跨域支持（CORS）
- 控制台中文输出（UTF-8），自动处理 Windows 代码页

---

## 📦 技术架构

```
┌─────────────────────────────────────────────┐
│  浏览器前端 (index.html + style.css + app.js)│
│  └ 液态玻璃 SVG 滤镜 / LG 物理引擎           │
└──────────────┬──────────────────────────────┘
               │ HTTP / SSE (每连接一线程)
┌──────────────▼──────────────────────────────┐
│  C++ 后端 (yuanbao.cpp + server.h)          │
│  ├ 多线程 acceptor + 每连接一线程           │
│  ├ Bot WebSocket 客户端 (SSL)               │
│  ├ MessageLogger 日志线程                   │
│  ├ FloodEngine 刷屏线程                     │
│  └ GlassEngine 液态玻璃引擎                 │
│    ├ glass.h   GLSL 着色器引擎              │
│    └ class.h   位移贴图/边缘环 C++ 组件      │
└──────────────┬──────────────────────────────┘
               │ WSS (腾讯元宝开放平台)
        ┌──────▼──────┐
        │  元宝 Bot    │
        └─────────────┘
```

---

## 🚀 快速开始

### 环境要求

| 依赖 | 说明 |
|---|---|
| C++17 编译器 | Windows: MinGW-w64 (winlibs 带 OpenSSL 版)；Linux/macOS: g++/clang++ |
| OpenSSL | 1.1+（WSS 加密连接必需） |
| 元宝开放平台凭证 | `APP_KEY` / `APP_SECRET` |

### 编译（Windows MinGW）

```bash
g++ -std=c++17 -O3 -Wall -I. yuanbao.cpp -o yuanbao_server.exe -lws2_32 -lssl -lcrypto
```

### 编译（Linux）

```bash
g++ -std=c++17 -O3 -Wall -I. yuanbao.cpp -o yuanbao_server -lpthread -lrt -lm -lssl -lcrypto
```

### 运行

```bash
# 先创建 config.json（见下方配置），然后：
./yuanbao_server          # 默认端口 8000
./yuanbao_server 9000     # 自定义端口
```

浏览器访问 **http://127.0.0.1:8000/** 打开控制台，点击「🔗 连接」即可。

---

## ⚙️ 配置（config.json）

> ⚠️ `config.json` 含 `APP_SECRET` 等敏感凭证，公开仓库会暴露密钥。上传前请确认是否包含真实凭证，或替换为占位符；切勿在公开环境中泄露真实 `APP_SECRET`。

```jsonc
{
  "PORT": 8000,                       // 服务端口
  "APP_KEY": "你的 APP_KEY",           // 元宝开放平台应用密钥
  "APP_SECRET": "你的 APP_SECRET",
  "API_DOMAIN": "bot.yuanbao.tencent.com",
  "WS_URL": "wss://bot-wss.yuanbao.tencent.com/wss/connection",
  "DEFAULT_GROUP_CODE": "群号",        // 默认目标群
  "YUANBAO_ID": "bot_xxxx",           // Bot 实例 ID
  "HEARTBEAT_INTERVAL": 10,
  // ── 大模型（可选）──
  "LLM_API_URL": "https://api.openai.com/v1/chat/completions",
  "LLM_API_KEY": "",
  "LLM_MODEL": "gpt-3.5-turbo",
  // ── 转发模式 ──
  "FORWARD_MODE_ENABLED": false,
  "FORWARD_AT_ONLY": false,
  "FORWARD_AT_YUANBAO": true,
  // ── 消息记录 ──
  "MSG_LOG_ENABLED": true,
  "RECALL_MONITOR_ENABLED": false
}
```

---

## 🧩 API 概览

| 分组 | 端点 |
|---|---|
| 连接 | `GET/POST /api/connect` `POST /api/disconnect` `GET /api/heartbeat` `GET /api/status` |
| 消息 | `POST /api/send` `POST /api/send-reply` `POST /api/send/at-all` `POST /api/send/ai-image` `POST /api/send-image` `POST /api/send-file` `POST /api/send-sticker` `GET /api/messages` |
| 群/成员 | `GET /api/groups` `POST /api/groups/switch` `GET /api/members` |
| 用户备注 | `GET/POST /api/users` `DELETE /api/users/{id}` |
| 贴纸 | `GET /api/stickers` |
| 大模型 | `GET/POST /api/llm/config` `POST /api/llm/test` |
| 转发模式 | `POST /api/forward-mode/{enable,disable,clear-queue,toggle-at-yuanbao}` `GET /api/forward-mode/config` |
| 消息日志 | `GET /api/msg-log/{stats,files,recent}` `POST /api/msg-log/{toggle,clear-today}` |
| 刷屏 | `POST /api/flood/{start,cancel}` `GET /api/flood/{stats,list}` |
| 液态玻璃 | `GET /api/glass/{vertex,blur,composite,presets,stats}` `POST /api/glass/compile` `GET /api/glass/displacement-map` `GET /api/glass/edge-ring` |
| 设置/配置 | `GET/POST /api/settings` |
| 插件 | `GET /api/plugins` `POST /api/plugins/{install,toggle,reload}` |

---

## 📁 目录结构

```
├── yuanbao.cpp          # 后端主程序（HTTP/WS 服务器、Bot 客户端、路由）
├── server.h             # 核心头文件（JSON、Protobuf、协议、主服务器类）
├── glass.h              # 液态玻璃 GLSL 着色器引擎（纯头文件）
├── class.h              # 液态玻璃高性能 C++ 组件（位移贴图/边缘环/PNG 编码）
├── index.html           # 前端页面
├── style.css            # 前端样式（含液态玻璃 CSS）
├── app.js               # 前端逻辑（LG 物理引擎、API 调用）
├── config.json          # 运行配置（含凭证，请勿在公开仓库暴露真实密钥）
├── logs/                # 消息日志目录（自动生成）
├── yuanbao_python/      # 参考用 Python 旧版实现（可选）
└── liquid-glass/        # 参考用液态玻璃组件库（已下载的开源参考）
```

---

## 📝 说明

- 本仓库为**纯 C++ 实现**，`yuanbao_python/` 为早期 Python 版本，仅作协议参考。
- `liquid-glass/` 目录包含下载的 **YanAndFish/liquid-glass**（Vue 3 液态玻璃组件库，MIT/其自带 LICENSE）与一个纯 HTML/CSS 液态玻璃参考实现，供视觉参考与对比。
- 前端界面不显示群号等敏感连接信息；连接信息请直接编辑 `config.json`。

## 🔒 许可证

[GNU Affero General Public License v3.0](./LICENSE)
