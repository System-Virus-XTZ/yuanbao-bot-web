# 元宝 Bot 统一服务器

> **当前版本：v1.0.1**
>
> 腾讯元宝开放平台 Bot 的一体化管理服务器 —— **纯 C++17 后端 + 现代 Web 前端**，无需 Python 即可运行。

浏览器打开即可连接元宝 Bot、收发消息、管理多个群聊、刷屏、发贴纸/图片/视频、接入大模型回复，内置 JSON 插件系统与多群同时监听能力。

---

## ✨ 功能特性

### 🤖 Bot 核心
- **WebSocket 长连接**：auth-bind 鉴权、心跳保活、断线自动重连
- **消息收发**：群消息、私聊（DM）、@ 指定成员、@ 全体、引用回复
- **实时消息流**：群消息经 SSE 实时推送到前端，缓存与日志历史合并展示
- **多群同时监听**：`LISTEN_GROUPS` 配置 + 前端逐群开关，被监听群的消息实时记录/展示/触发回复
- **已加入群列表**：收到任意群消息自动收集群号并持久化（`known_groups.json`），无需手动维护

### 📤 发送能力
- **文本 / @ 成员**：支持单 @、多 @、@ 全体
- **引用回复**：回复指定消息（`/api/send-reply`）
- **贴纸**：内置 80+ 腾讯表情，支持贴纸 + 文字 + @，可设置**次数与间隔批量刷屏**
- **图片**：URL 图片或本地文件上传发送
- **文件 / 视频**：本地文件上传（最大 20MB，视频以文件消息发送）

### ⚡ 增强能力
- **刷屏引擎**：15 种文本变换（全角 / MOCK / 乱码 / 彩虹 / 地震 / 矩阵等），自定义次数与间隔，后台线程执行不阻塞响应
- **大模型回复**：接入 OpenAI 兼容 API（DeepSeek / 通义千问 / 智谱等），群消息智能自动回复，可设置 system prompt / 温度 / 超时
- **代理转发模式**：非元宝群的群消息转发到元宝群并 @ 元宝，元宝的回复按 FIFO 队列自动回传到原群（引用原消息）；支持「仅 @ 元宝 时转发」
- **撤回监控**：检测群撤回事件，发送「撤回通知」（撤回者 / 原发送者 / 时间 / 原内容）；图片被撤回自动补发原图
- **图片消息展示**：解析图片 `media_info`，前端直接渲染缩略图

### 🧩 插件系统
- **JSON 声明式插件**：扫描 `plugins/*.json`，定义「命令 → 回复」规则，收到群消息匹配即回复
- 命令匹配**优先于 AI 回复**；支持 `@元宝 /ping` 形式与 `{user}` 占位
- 前端插件面板可安装（粘贴 JSON 或 URL 下载）、启用 / 禁用、重载

### 👥 群成员管理
- 实时获取群成员列表、群主标记、昵称备注（`/api/users`）
- 切换主群 / 监听群后自动清理旧缓存，重新获取

### 📋 消息日志
- JSONL + 文本双格式落盘
- 历史消息回放、按日统计、下载、清空

### 📱 现代 Web 前端
- 现代视觉风格，响应式布局
- **手机端适配**：竖排恢复条导航（左右栏折叠入口）、栏内返回按钮、发送面板吸底置顶操作条等宽

---

## 🚀 快速开始

### 环境要求

| 依赖 | 说明 |
|---|---|
| C++17 编译器 | Windows: MinGW-w64（winlibs 带 OpenSSL 版）；Linux/macOS: g++/clang++ |
| OpenSSL | 1.1+（WSS 加密连接必需） |
| 元宝开放平台凭证 | `APP_KEY` / `APP_SECRET` |

### 编译（Windows MinGW）

```bash
g++ -std=c++17 -O2 -Wall -I. yuanbao.cpp -o yuanbao_server.exe -lws2_32 -lssl -lcrypto
```

### 编译（Linux）

```bash
g++ -std=c++17 -O2 -Wall -I. yuanbao.cpp -o yuanbao_server -lpthread -lrt -lm -lssl -lcrypto
```

### 运行

```bash
# 先创建 config.json（见下方配置），然后：
./yuanbao_server            # 默认端口 8000
./yuanbao_server 9000       # 自定义端口
```

浏览器访问 **http://127.0.0.1:8000/** 打开控制台，点击「🔗 连接」即可。

> Windows 下也提供了预编译的 `yuanbao_server.exe`，可直接运行。

---

## ⚙️ 配置（config.json）

> ⚠️ **安全提醒**：`config.json` 含 `APP_SECRET` 等真实凭证，**切勿提交到公开仓库**。演示/示例请使用占位符。

```jsonc
{
  "PORT": 8000,                            // 服务端口
  "APP_KEY": "你的 APP_KEY",               // 元宝开放平台应用密钥
  "APP_SECRET": "你的 APP_SECRET",
  "API_DOMAIN": "bot.yuanbao.tencent.com",
  "WS_URL": "wss://bot-wss.yuanbao.tencent.com/wss/connection",
  "DEFAULT_GROUP_CODE": "主群号",          // 默认主群（必须，且始终在监听列表）
  "LISTEN_GROUPS": [],                    // 多群监听列表：可留空，首次运行时自动同步所有已加入群
  "YUANBAO_ID": "",                       // Bot 实例 ID：可留空，连接鉴权成功后自动获取并回写
  "HEARTBEAT_INTERVAL": 10,
  // ── 大模型（可选）──
  "LLM_API_URL": "https://api.openai.com/v1/chat/completions",
  "LLM_API_KEY": "",
  "LLM_MODEL": "gpt-3.5-turbo",
  "LLM_SYSTEM_PROMPT": "你是一个友好的QQ群聊机器人助手……",
  "LLM_MAX_TOKENS": 200,
  "LLM_TEMPERATURE": 0.8,
  "LLM_TIMEOUT": 15,
  // ── 代理转发模式 ──
  "FORWARD_MODE_ENABLED": false,
  "FORWARD_AT_ONLY": false,               // 仅 @元宝 时转发
  "FORWARD_AT_YUANBAO": true,             // 转发时 @元宝 确保回复
  // ── 消息记录 / 撤回监控 ──
  "MSG_LOG_ENABLED": true,
  "RECALL_MONITOR_ENABLED": false
}
```

> **自动获取**：`YUANBAO_ID` 连接鉴权成功后自动回写；`LISTEN_GROUPS` 留空时启动自动同步所有已加入群，收到新群消息自动加入监听。两者均可在前端「元宝派」面板逐群开关，运行时改动自动写回 `config.json`。

---

## 🧩 API 概览

| 分组 | 端点 |
|---|---|
| 连接 | `GET/POST /api/connect` `POST /api/disconnect` `GET /api/bot_status` `GET /api/status` `GET /api/health` |
| 心跳 | `GET /api/heartbeat` `POST /api/heartbeat/interval` |
| 发送 | `POST /api/send` `POST /api/send-reply` `POST /api/send-at` `POST /api/send-multi-at` `POST /api/send/at-all` `POST /api/send/ai-image` `POST /api/send-image` `POST /api/send-sticker`（含 count/interval 刷屏） `POST /api/send-file` |
| 消息 | `GET /api/messages` `POST /api/messages/clear` `GET /api/messages/logs` |
| 群 / 监听 | `GET /api/groups` `POST /api/groups/switch` `POST /api/groups/listen` `GET /api/members` |
| 贴纸 | `GET /api/stickers` |
| 刷屏 | `POST /api/flood/start` `POST /api/flood/cancel` `GET /api/flood/stats` `GET /api/flood/list` |
| 代理转发 | `POST /api/forward-mode/{enable,disable,clear-queue,toggle-at-yuanbao}` `GET /api/forward-mode/{config,queue}` |
| 大模型 | `GET/POST /api/llm/config` `POST /api/llm/test` |
| 消息日志 | `GET /api/msg-log/{stats,files,recent}` `POST /api/msg-log/{enable,disable,toggle,clear-today}` `GET /api/msg-log/download` |
| 插件 | `GET /api/plugins` `POST /api/plugins/{install,toggle,reload}` |
| 设置 | `GET/POST /api/settings` |
| SSE | `GET /api/events` `/events` |

---

## 🧩 插件系统（JSON 配置插件）

Python 版可用 `importlib` 动态加载代码，C++ 无等价机制，因此采用**声明式 JSON 插件**：

- 每个插件一个 JSON 文件，放在 `plugins/` 目录（自动创建），命名 `插件名.json`
- 后端启动 / `/api/plugins/reload` 时自动扫描加载
- 收到监听群消息时，内容以命令前缀开头即自动回复对应文本
- 命令匹配**优先于 AI 回复**；`@昵称 命令` 形式也会被识别

```json
{
  "name": "example",
  "version": "1.0.0",
  "author": "yuanbao",
  "description": "示例插件：/ping 回复 pong",
  "active": true,
  "commands": [
    { "command": "/ping", "reply": "pong! 🏓" },
    { "command": "/time", "reply": "现在是北京时间" }
  ]
}
```

字段说明：`name`（必须，即文件名）/ `version` / `author` / `description` / `active`（默认 true）/ `commands`（命令列表，必填）。

- `command`：命令前缀，消息以它开头即触发（支持 `/ping` 或 `@元宝 /ping`）
- `reply`：回复文本，支持 `{user}` 占位

管理方式：前端「设置 → 插件」页可安装（粘贴 JSON 或 URL 下载）、启用 / 禁用 / 重载；也可直接放 JSON 文件到 `plugins/` 后调 `/api/plugins/reload`。仓库附 `plugins/example.json` 示例与 `plugins/README.md` 说明。

---

## 📁 目录结构

```
├── yuanbao.cpp          # 后端主程序（HTTP/SSE 服务器、路由、Bot 逻辑）
├── server.h             # 核心头文件（JSON / 协议 / BotConfig / 主服务器类）
├── yuanbao_client.h     # Bot WebSocket 客户端（鉴权、心跳、收发帧）
├── index.html           # 前端页面
├── style.css            # 前端样式（响应式）
├── app.js               # 前端逻辑（API 调用、实时消息流）
├── config.json          # 运行配置（含真实凭证，请勿提交公开仓库）
├── plugins/             # JSON 配置插件目录（含 example.json 与格式说明）
├── yuanbao-python/      # Python 版参考实现（可选）
├── yuanbao_server.exe   # Windows 预编译可执行文件
└── LICENSE
```

---

## 📝 说明

- 本仓库为**纯 C++ 实现**，无需 Python 运行时即可运行完整 Web 服务。
- 前端界面不显示密钥等敏感信息；连接与群配置请直接编辑 `config.json`。
- `LISTEN_GROUPS` 与主群 `DEFAULT_GROUP_CODE` 的关系：主群始终在监听列表且**不可取消**；其余监听群可随时在前端开关。

## 🔒 许可证

[GNU Affero General Public License v3.0](./LICENSE)
