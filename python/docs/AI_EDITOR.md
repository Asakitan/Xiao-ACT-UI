# AI Editor 架构

> 当前版本：`5.0.0`。配套文档：`docs/ACT_PLATFORM.md`、`docs/PLUGIN_SDK.md`。

SAO ACT UI 内置的 AI 编辑器是一个**独立 pywebview 窗口**，提供多 Provider LLM 对话、VSCode 风格的 tool calling、MCP 服务器集成、VSCode Marketplace 扩展浏览器，以及 dark/light 双主题。

从 SAO 菜单 → ACT → "AI Editor (LLM)" 打开。也可命令行 `python -m ai_editor.app` 独立启动。

## 模块清单

| 文件 | 职责 |
|------|------|
| `prompts.py` | System prompt — 项目简介、工具表、IDE 使用指南、Agent Mode 指令 |
| `llm_engine.py` | 多 Provider LLM 引擎（OpenAI / Anthropic 原生 / DeepSeek / Ollama / 自定义兼容端点） |
| `tool_registry.py` | 工具注册中心、OpenAI function-calling schema 生成、参数自动推断、decorator API |
| `engine_tools.py` | VSCode 对齐工具集 + `engine` 聚合入口 |
| `chat_state.py` | 对话管理、send → stream → tool-call → resume 循环、@-mention 解析、Agent Mode |
| `app.py` | pywebview 启动器 + `AIEditorAPI` JS bridge（30+ 方法） |
| `mcp_client.py` | MCP (Model Context Protocol) 客户端：stdio + HTTP/SSE 双传输 |
| `extensions.py` | VSCode Marketplace API 客户端 + 扩展 tool 加载 |
| `history.py` | 对话历史持久化（JSON 文件、原子写入） |
| `selftest.py` | 自测套件 |

## LLM 通讯协议

### OpenAI Chat Completions（默认）

适用于 OpenAI、DeepSeek、Ollama、vLLM、SiliconFlow 等所有 OpenAI 兼容端点。

```
POST {base_url}/chat/completions
Content-Type: application/json
Authorization: Bearer {api_key}

{
  "model": "gpt-4o",
  "messages": [...],
  "tools": [...],          // OpenAI function-calling schema
  "stream": true,
  "stream_options": {"include_usage": true}
}

SSE 响应:
  data: {"choices":[{"delta":{"content":"..."}}]}
  data: {"choices":[{"delta":{"tool_calls":[...]}}]}
  data: {"choices":[{"delta":{"reasoning_content":"..."}}]}  // DeepSeek R1
  data: [DONE]
```

### Anthropic Messages API（原生）

当 provider=anthropic 且 base_url 含 `anthropic.com` 时自动切换。

```
POST {base_url}/messages
Content-Type: application/json
x-api-key: {api_key}
anthropic-version: 2023-06-01

{
  "model": "claude-sonnet-4-20250514",
  "system": "...",
  "messages": [...],       // tool_result → user role
  "tools": [...],          // input_schema 格式
  "stream": true,
  "max_tokens": 4096
}

SSE 响应:
  event: message_start     data: {"type":"message_start",...}
  event: content_block_start data: {"type":"content_block_start","content_block":{"type":"thinking"}}
  event: content_block_delta data: {"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"..."}}
  event: content_block_delta data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"..."}}
  event: content_block_start data: {"content_block":{"type":"tool_use","id":"toolu_...","name":"readFile"}}
  event: content_block_delta data: {"delta":{"type":"input_json_delta","partial_json":"..."}}
  event: message_delta     data: {"delta":{"stop_reason":"end_turn"},"usage":{...}}
  event: message_stop
```

### 消息格式转换

Anthropic 原生模式下，`llm_engine.py` 自动处理：
- `system` 消息从 messages 提取为顶级 `system` 字段
- `tool` 角色转为 `user` 角色的 `tool_result` content block
- `assistant` 的 `tool_calls` 转为 `tool_use` content block
- `thinking` content block 解析并传播到 `ChatMessage.thinking`

## 工具系统

工具对齐 VSCode Copilot 的设计。12 个工具，5 个分类：

### 文件操作（file）

| 工具 | 功能 |
|------|------|
| `readFile(path, startLine?, endLine?)` | 读取文件内容 |
| `editFile(path, content, startLine?, endLine?)` | 创建或编辑文件 |
| `listFiles(path, pattern?, recursive?)` | 列出目录 |
| `searchFiles(query, path?, pattern?, regex?, caseSensitive?)` | grep 搜索 |

### 终端（terminal）

| 工具 | 功能 |
|------|------|
| `runTerminal(command, cwd?)` | 执行 shell 命令（30s 超时） |

### 交互（interaction）

| 工具 | 功能 |
|------|------|
| `askQuestion(question)` | 向用户提问 |
| `taskComplete(summary)` | 标记任务完成 |
| `getConfirmation(action, risk?)` | 确认危险操作 |

### 编辑器（editor）

| 工具 | 功能 |
|------|------|
| `editor_getContent()` | 读取编辑器内容 |
| `editor_setContent(content, language?)` | 写入编辑器 |
| `editor_getSelection()` | 获取选中文本 |

### 引擎聚合（engine）

单一入口 `engine(action, ...)` 分发到平台和插件：

**平台 action**（始终可用）：`system_info`、`plugins`、`settings_get`、`settings_set`、`memory_status`、`eval`、`exec`

**Star Resonance 插件 action**（加载 SR 时可用）：`game_state`、`entity_list`、`dps_summary`、`dps_report`、`boss_status`、`combat_status`、`buff_list`、`auto_key_status`

## MCP 集成

`mcp_client.py` 实现 JSON-RPC 2.0 over stdin/stdout 和 HTTP/SSE 两种传输。

### 配置来源

1. `settings.ai_editor_mcp_servers` 列表
2. 工作区 `mcp.json` / `.vscode/mcp.json` / `.mcp/mcp.json`

### 协议

```
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}
← {"jsonrpc":"2.0","id":1,"result":{"capabilities":{...}}}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"...","arguments":{...}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"..."}]}}
```

MCP 工具自动注册为 `mcp_{server}_{tool}` 出现在 LLM 工具列表中。

## 扩展市场

`extensions.py` 直接对接 VSCode Marketplace API：

```
POST https://marketplace.visualstudio.com/_apis/public/gallery/extensionquery
Accept: application/json;api-version=6.1-preview.1
```

安装流程：搜索 → 下载 VSIX → 提取 package.json → 解析 `contributes`（languageModelTools / chatParticipants / commands）→ 注册为可用工具。

## UI 特性

HTML GUI (`web/ai_editor_app.html`) 完整复刻 VSCode 布局：

- **Activity Bar**：Chat / Tools / Extensions / History / Theme / Settings
- **Sidebar**：模型选择器、快捷操作、工具列表、扩展市场、对话历史
- **Editor**：多 Tab、行号、语法高亮（20 种语言）、Find/Replace（正则/大小写/全词）、右键菜单、Ctrl+G/Ctrl+//Ctrl+D 等快捷键
- **Chat Panel**：流式 markdown（word-by-word fade）、代码块语法高亮 + Copy、可折叠 thinking 块（shimmer 动画）、可折叠 tool call/result（spinner→checkmark）、tool 确认栏（Allow/Deny）、image 上传
- **Terminal Panel**：shell 命令输入+执行+stdout/stderr 显示
- **Status Bar**：Ln/Col、Spaces、UTF-8、LF、Language、Model、Tokens
- **Command Palette**：Ctrl+Shift+P，19 个命令
- **Toast 通知**：info/success/error/warning
- **Dark/Light 主题**：`[data-theme]` CSS 变量双套，同步 ACT `panel_themes.act`

## 主题系统

Dark 和 Light 各定义完整的 CSS 变量集。accent 颜色与 ACT 平台对齐：

| 变量 | Dark | Light |
|------|------|-------|
| `--bg` | `#1e1e1e` | `#f5f5f5` |
| `--fg` | `#cccccc` | `#3b3a3c` |
| `--fg-accent` | `#68e4ff` | `#16a9d6` |
| `--border` | `#3c3c3c` | `#d0d0d0` |
| `--statusbar-bg` | `#007acc` | `#007acc` |

主题从 `ai_editor.theme` 设置或 ACT `panel_themes.act` 读取，通过 `document.documentElement.setAttribute('data-theme', theme)` 切换。

## @-mention 上下文变量

输入 `@` 触发补全弹窗：

| 变量 | 解析内容 |
|------|---------|
| `@file` | 当前编辑器文件名 |
| `@selection` | 编辑器选中文本 |
| `@editor` | 编辑器全部内容（截断 2000 字符） |
| `@state` | 游戏状态（via engine(game_state)） |
| `@language` | 当前编辑器语言模式 |

## Agent Mode

勾选 Agent Mode 后：
- System prompt 追加 plan→execute→verify→report 指令
- `MAX_TOOL_ROUNDS` 从 10 提高到 25
- 响应以 `...` 或 `[continue]` 结尾时自动注入 "Continue with the next step"

## 验证

```bash
python -m ai_editor.selftest     # 59 项自测
python -m ai_editor.app           # 独立启动 GUI
```
