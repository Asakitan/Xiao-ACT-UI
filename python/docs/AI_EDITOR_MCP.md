# AI Editor MCP Server

SAO AI Editor 内置 MCP (Model Context Protocol) 服务器，允许外部 IDE 的 AI 助手（Claude Code、GitHub Copilot、Cursor、Windsurf 等）直接接入并使用 AI Editor 的全部能力。

---

## 快速开始

### 1. 确认 Python 环境

MCP 服务器运行在 `sao_auto/python/` 目录下，需要能 import `ai_editor` 包：

```bash
cd E:\VC\SAO-UI\sao_auto\python
python -m ai_editor.mcp_server --help
```

### 2. 配置 IDE 接入

根据你使用的 IDE，在对应位置添加 MCP 配置。

---

## IDE 接入配置

### Claude Code

在项目根目录创建 `mcp.json`，或添加到 `~/.claude.json`：

```json
{
  "mcpServers": {
    "sao-ai-editor": {
      "command": "python",
      "args": ["-m", "ai_editor.mcp_server"],
      "cwd": "E:\\VC\\SAO-UI\\sao_auto\\python"
    }
  }
}
```

也可以用 Claude Code CLI 直接添加：

```bash
claude mcp add sao-ai-editor -- python -m ai_editor.mcp_server
```

### VS Code (Copilot / Continue / Cline)

在项目中创建 `.vscode/mcp.json`：

```json
{
  "servers": {
    "sao-ai-editor": {
      "type": "stdio",
      "command": "python",
      "args": ["-m", "ai_editor.mcp_server"],
      "cwd": "E:\\VC\\SAO-UI\\sao_auto\\python"
    }
  }
}
```

### Cursor

在 Cursor Settings → MCP 中添加：

```json
{
  "mcpServers": {
    "sao-ai-editor": {
      "command": "python",
      "args": ["-m", "ai_editor.mcp_server"],
      "cwd": "E:\\VC\\SAO-UI\\sao_auto\\python"
    }
  }
}
```

### Windsurf / 其他支持 MCP 的工具

同上格式，在工具各自的 MCP 配置位置添加即可。

### HTTP 模式（远程/共享）

如果需要多个 IDE 同时连接同一个服务器实例：

```bash
python -m ai_editor.mcp_server --port 9820
```

客户端用 SSE 方式连接 `http://127.0.0.1:9820`。

---

## 暴露的工具 (Tools)

MCP 服务器暴露以下 18 个工具，覆盖 AI Editor 的全部核心能力：

### 文件操作

| 工具 | 说明 |
|------|------|
| `read_file` | 读取文件内容，支持行范围 `start_line`/`end_line` |
| `edit_file` | 创建/编辑文件，支持全量写入或行范围替换 |
| `list_files` | 列出目录内容，支持 glob 过滤和递归 |
| `search_files` | 全文搜索/正则搜索，返回匹配行 |

### 终端

| 工具 | 说明 |
|------|------|
| `run_terminal` | 执行 shell 命令，返回 stdout/stderr |

### LLM 对话

| 工具 | 说明 |
|------|------|
| `chat` | 发送消息到 AI Editor 配置的 LLM，获取回复。支持覆盖 model/temperature/max_tokens/system_prompt |
| `chat_with_agent` | 使用指定 Agent 身份发送消息（如 code-reviewer、debugger 等） |
| `run_workflow` | 执行多步骤 AI 工作流（如 review-and-fix、debug-trace 等） |

### 注册表查询

| 工具 | 说明 |
|------|------|
| `list_agents` | 列出所有可用 Agent（内置 + 自定义） |
| `list_workflows` | 列出所有可用工作流 |

### 平台引擎

| 工具 | 说明 |
|------|------|
| `engine` | ACT 平台引擎操作：system_info, plugins, settings_get/set, memory_status, list_processes, select_process, eval, exec |
| `sdk_dumper` | 游戏引擎 SDK 导出：从运行进程内存中提取 IL2CPP/Mono/Unreal/Source 的类/字段/方法 |

### 网络

| 工具 | 说明 |
|------|------|
| `web_fetch` | HTTP 请求，支持 GET/POST + 自定义 headers |

### 配置

| 工具 | 说明 |
|------|------|
| `get_config` | 读取 AI Editor 配置（provider/mcp/terminal/agents/workflows） |
| `set_config` | 修改 AI Editor 配置项 |

### 指令系统

| 工具 | 说明 |
|------|------|
| `get_instructions` | 获取自定义指令（system + workspace + plugin 三级） |
| `save_instructions` | 保存自定义指令文件 |

### MCP 元数据

| 工具 | 说明 |
|------|------|
| `list_mcp_servers` | 列出 AI Editor 自身连接的 MCP 子服务器 |

---

## 使用示例

### 在 Claude Code 中使用

配置完成后，Claude Code 会自动发现 MCP 工具。你可以直接在对话中使用：

```
> 用 code-reviewer agent 审查 ai_editor/mcp_server.py

Claude 会自动调用 chat_with_agent 工具：
  agent_id: "code-reviewer"
  message: "审查以下代码... [文件内容]"
```

```
> 查看 AI Editor 当前配置的 LLM 提供商

Claude 会调用 get_config 工具：
  section: "provider"
```

```
> 列出所有可用的 AI 工作流

Claude 会调用 list_workflows 工具
```

```
> 用 debug-trace 工作流分析这个错误
[粘贴错误信息]

Claude 会调用 run_workflow 工具：
  workflow_id: "debug-trace"
  input: "[错误信息]"
```

### 平台引擎操作

```
> 查看 SAO ACT 的系统状态

Claude 调用 engine：action="system_info"
→ 返回版本、运行时间、数据源模式
```

```
> 列出当前运行的游戏进程

Claude 调用 engine：action="list_processes"
```

```
> 从 PID 12345 导出 IL2CPP SDK

Claude 调用 sdk_dumper：action="dump", pid=12345
```

---

## 架构

```
┌─────────────────────────────────────────────┐
│              IDE (Claude Code)              │
│                                             │
│  AI 发现 MCP 工具 → 按需调用               │
└────────────────┬────────────────────────────┘
                 │ stdio (JSON-RPC 2.0)
                 │ 或 HTTP (:9820)
┌────────────────▼────────────────────────────┐
│         ai_editor.mcp_server                │
│                                             │
│  McpServer (stdio)  或  McpHttpServer (HTTP)│
│         │                                   │
│         ▼                                   │
│  McpRuntime (headless)                      │
│  ├── LLMEngine (多 provider 支持)           │
│  ├── ToolRegistry (文件/终端/引擎工具)      │
│  ├── AgentRegistry (5 内置 + 自定义)        │
│  ├── WorkflowRegistry (3 内置 + 自定义)     │
│  ├── McpManager (子 MCP 服务器)             │
│  └── Settings (配置读写)                    │
└─────────────────────────────────────────────┘
```

### 两种运行模式

**Stdio 模式**（默认）：IDE 启动 MCP 服务器作为子进程，通过 stdin/stdout 通信。每个 IDE 连接一个独立实例。

```bash
python -m ai_editor.mcp_server
```

**HTTP 模式**：作为长驻 HTTP 服务器运行，多个客户端可以同时连接。

```bash
python -m ai_editor.mcp_server --port 9820
```

### 无 GUI 依赖

MCP 服务器以 headless 模式运行，不依赖 pywebview 或 Tk GUI。它直接初始化：
- LLM 引擎（从 settings.json 读取 provider/api_key/model 配置）
- 工具注册表
- Agent 和 Workflow 注册表
- MCP 客户端管理器（如果配置了子 MCP 服务器）

---

## 配置前置条件

### LLM 功能

要使用 `chat`、`chat_with_agent`、`run_workflow` 等 LLM 相关工具，需要在 AI Editor 设置中配置好 LLM 提供商：

1. 打开 SAO ACT 主界面 → 工具 → AI Editor
2. 点击 ⚙ 设置 → 配置 provider / api_key / model
3. 保存后 MCP 服务器会自动读取这些配置

或者直接编辑 `settings.json`：

```json
{
  "ai_editor": {
    "provider": "openai",
    "api_key": "sk-...",
    "model": "gpt-4o",
    "temperature": 0.7,
    "max_tokens": 4096
  }
}
```

### 文件/终端功能

这些工具开箱即用，不需要额外配置。

### SDK Dumper

需要 `ai_editor.sdk_dumper` 模块可用（项目自带）。目标游戏进程必须正在运行。

---

## 协议细节

- 协议版本：`2024-11-05`
- 传输：stdio (JSON-RPC 2.0 + Content-Length 头) 或 HTTP POST
- 服务器名：`sao-ai-editor`
- 支持的 MCP 方法：
  - `initialize` / `notifications/initialized`
  - `tools/list` / `tools/call`
  - `resources/list`（返回空）
  - `prompts/list`（返回空）
  - `ping`

---

## 故障排查

### MCP 服务器无法启动

```bash
# 检查 Python 路径和 import
cd E:\VC\SAO-UI\sao_auto\python
python -c "from ai_editor.mcp_server import McpServer; print('OK')"
```

### LLM 工具返回 "not configured"

确保 `settings.json` 中有有效的 `ai_editor.provider` 和 `ai_editor.api_key`。

### 日志查看

MCP 服务器的日志输出到 stderr。在 Claude Code 中可以通过 `claude mcp logs sao-ai-editor` 查看。

### 工具列表验证

```bash
# 快速测试 tools/list 响应
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | python -m ai_editor.mcp_server 2>/dev/null | head -1
```

---

## 自定义扩展

### 添加自定义 Agent

在 `.sao/agents/` 目录下创建 JSON 文件：

```json
{
  "id": "my-analyst",
  "name": "My Analyst",
  "description": "Analyzes game data patterns",
  "system_prompt": "You are a game data analyst...",
  "icon": "📊",
  "when_to_use": "When analyzing game mechanics or data patterns"
}
```

MCP 工具 `list_agents` 会自动发现，`chat_with_agent` 可以直接使用。

### 添加自定义 Workflow

在 `.sao/workflows/` 目录下创建 JSON 文件：

```json
{
  "id": "full-review",
  "name": "Full Code Review",
  "description": "Review + Security Audit + Performance Check",
  "steps": [
    {"agent": "code-reviewer", "prompt": "Review:\n\n{{input}}", "output_var": "review", "label": "Code Review"},
    {"agent": "optimizer", "prompt": "Based on review:\n{{review}}\nCheck performance.", "output_var": "perf", "label": "Perf Check"}
  ]
}
```

### 嵌套 MCP

AI Editor 自身也是 MCP 客户端。配置子 MCP 服务器后，`list_mcp_servers` 工具会显示连接的服务器，IDE AI 可以通过 AI Editor 的 LLM 间接使用这些子工具。
