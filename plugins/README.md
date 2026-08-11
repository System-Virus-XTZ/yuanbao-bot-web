# C++ 版插件系统（JSON 配置插件）

Python 版可用 `importlib` 动态加载代码，C++ 无等价机制，因此采用**声明式 JSON 插件**：

- 每个插件一个 JSON 文件，放在 `plugins/` 目录，命名 `插件名.json`
- 后端启动 / `/api/plugins/reload` 时自动扫描加载
- 收到监听群消息时，若内容以某命令前缀开头，自动回复对应文本
- 命令匹配**优先于 AI 回复**；`@昵称 命令` 形式（消息开头带 @）也会被识别

## 格式

```json
{
  "name": "example",
  "version": "1.0.0",
  "author": "yuanbao",
  "description": "示例插件",
  "active": true,
  "commands": [
    { "command": "/ping", "reply": "pong! 🏓" },
    { "command": "/time", "reply": "现在是北京时间" }
  ]
}
```

字段说明：

| 字段 | 说明 |
|------|------|
| `name` | 插件名（必须，也是文件名） |
| `version` | 版本号，默认 1.0.0 |
| `author` | 作者 |
| `description` | 描述 |
| `active` | 是否启用（默认 true） |
| `commands` | 命令列表（必填，至少 1 条） |

`commands` 项：

| 字段 | 说明 |
|------|------|
| `command` | 命令前缀，消息以它开头即触发（支持 `/ping` 或 `@元宝 /ping`） |
| `reply` | 回复文本，支持 `{user}` 占位（替换为「你」） |

## 管理

- 前端「设置 → 插件」页可安装 / 启用 / 禁用 / 重载插件
- 安装：直接粘贴插件 JSON 内容，或提供指向 raw JSON 的 URL
- 手动安装：把 JSON 文件放进 `plugins/` 目录后，调用 `/api/plugins/reload` 或重启服务
