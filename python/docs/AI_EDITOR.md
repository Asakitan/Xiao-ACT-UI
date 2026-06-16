# AI Editor 架构

> 当前版本：`5.0.0`。配套文档：`docs/ACT_PLATFORM.md`、`docs/PLUGIN_SDK.md`。

SAO ACT UI 内置的 AI 编辑器是一个**独立 pywebview 窗口**，提供多 Provider LLM 对话、VSCode 风格的 tool calling、MCP 服务器集成、VSCode Marketplace 扩展浏览器，以及 dark/light 双主题。

从 SAO 菜单 → ACT → "AI Editor (LLM)" 打开。也可命令行 `python -m ai_editor.app` 独立启动。

## 模块清单

| 文件 | 职责 |
|------|------|
| `prompts.py` | VSCode Copilot 风格 system prompt（模块化拼接: _IDENTITY + _TOOL_RULES + _MEM_PROBE_GUIDE + _SAFETY + _PROJECT_STRUCTURE）|
| `llm_engine.py` | 多 Provider LLM 引擎（OpenAI / Anthropic 原生 SSE / 兼容端点），持久 httpx 连接池 |
| `tool_registry.py` | 工具注册中心、OpenAI function-calling schema 生成、参数自动推断 |
| `engine_tools.py` | 12 个 VSCode 对齐工具 + `engine` 聚合入口（15 个 sub-action） |
| `chat_state.py` | 对话管理、tool 循环、@-mention（预编译正则）、Agent Mode、to_api_messages 缓存 |
| `app.py` | pywebview 启动器 + `AIEditorAPI` JS bridge（30+ 方法），stream delta 批量合并 |
| `mcp_client.py` | MCP 客户端：stdio + HTTP/SSE + **内部 Python 注册**（InternalMcpProvider） |
| `extensions.py` | VSCode Marketplace API 客户端 + VSIX tool 加载，共享 httpx 连接池 |
| `history.py` | 对话历史持久化（JSON 文件、原子写入、版本号跳过无变更保存） |
| `selftest.py` | 59 项自测套件 |

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

`mcp_client.py` 支持三种传输模式：

| 模式 | 场景 | 示例 |
|------|------|------|
| **stdio** | 外部进程, JSON-RPC over stdin/stdout | `npx @modelcontextprotocol/server-filesystem` |
| **sse** | 远程 HTTP+SSE 端点 | `https://mcp.example.com/sse` |
| **internal** | Python 插件直接注册, 无子进程 | 插件 `on_load` 时调用 |

### 配置来源 (优先级)

1. `settings.ai_editor_mcp_servers` 列表
2. 工作区 `mcp.json` / `.vscode/mcp.json` / `.mcp/mcp.json`
3. 用户主目录 `~/.sao/mcp.json`
4. 插件 manifest `plugins/*/plugin.json` → `mcpServers`

### 协议 (stdio/sse)

```
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}
← {"jsonrpc":"2.0","id":1,"result":{"capabilities":{...}}}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"...","arguments":{...}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"..."}]}}
```

### 内部 MCP (插件注册)

```python
# 在 plugin.py on_load(ctx) 中:
from ai_editor.mcp_client import InternalMcpProvider

provider = InternalMcpProvider("my_plugin")
provider.add_tool(
    "get_hp", "Read player HP",
    {"type": "object", "properties": {}},
    handler=lambda: {"hp": 50000},
)
# 注册到 MCP manager
mcp_manager.register_provider(provider)
```

或通过 app API 批量注册:

```python
mcp_manager.register_internal("my_game", [
    {"name": "scan_memory", "description": "Scan for value",
     "inputSchema": {"type":"object","properties":{"value":{"type":"integer"}}}},
], handlers={"scan_memory": lambda value=0: {"found": 3}})
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

## mem_probe 集成

mem_probe 是通用内存扫描基础设施（游戏无关）。system prompt 教 LLM 如何使用：

- `mem_probe.process.GameProcess` — 附加目标进程（进程名从 config，不硬编码）
- `mem_probe.scanner` — 多帧值搜索 scan→narrow
- `mem_probe.cy_memscan` — AVX2 加速（Cython，有纯 Python fallback）
- `mem_probe.unified_source` — TCP/内存混合数据源，插件通过 `set_bridge_classes()` 注入桥接

LLM 通过 `engine(action="eval/exec")` 直接操作内存：

```
engine(action="exec", code="from mem_probe.process import GameProcess; ...")
```

## Chat 交互功能

对齐 VSCode Copilot 的用户交互：

- **代码块 toolbar** — Copy📋 / Insert📥 / Run▶ / New Tab📄（hover 显示）
- **消息 footer** — 👍/👎 评分 + 📋复制 + 🔄重试
- **Follow-up 建议** — 响应后自动生成上下文相关建议
- **文件拖拽附件** — 拖入文件 → pill 显示 → 发送时 prepend
- **消息右键菜单** — Copy / Insert to Editor / Run in Terminal / Delete
- **命令面板** — Ctrl+Shift+P，19 个命令
- **Toast 通知** — info/success/error/warning

## 性能优化

已修复的关键瓶颈：

| 优化 | 模块 | 效果 |
|------|------|------|
| httpx 持久连接池 | llm_engine, extensions, mcp_client | -400ms/请求 |
| stream delta 批量合并 | app.py | -93% JS eval 调用 |
| to_api_messages 版本缓存 | chat_state | -90% 消息拷贝 |
| @-mention 正则预编译 | chat_state | 消除重复编译 |
| auto-save 版本号跳过 | chat_state | -50% 磁盘写 |
| editor_get_content 单次 JS eval | app.py | -50% 延迟 |
| MCP stdio bufsize 8192 | mcp_client | -90% 系统调用 |

## 验证

```bash
python -m ai_editor.selftest     # 59 项自测
python -m ai_editor.app           # 独立启动 GUI
```
