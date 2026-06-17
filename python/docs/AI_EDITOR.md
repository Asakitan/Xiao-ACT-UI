# AI Editor 架构

> 当前版本：`5.1.0`。配套文档：`docs/ACT_PLATFORM.md`、`docs/PLUGIN_SDK.md`。

SAO ACT UI 内置的 AI 编辑器是一个**独立 pywebview 窗口**，提供多 Provider LLM 对话、VSCode 风格布局与工具系统、三层 scope 体系、自定义 Agent/Workflow、Chat Provider 动态注册、模型级 context window 管理、MCP 集成、VSCode Marketplace 扩展浏览器，以及 dark/light 双主题。

从 SAO 菜单 → ACT → "AI Editor (LLM)" 打开。也可命令行 `python -m ai_editor.app` 独立启动。

## 模块清单

| 文件 | 职责 |
|------|------|
| `prompts.py` | System prompt 模块化拼接 + 三 scope 指令加载 + Agent/Workflow 描述注入 |
| `llm_engine.py` | 多 Provider LLM 引擎（OpenAI / Anthropic 原生 SSE），全量采样参数，模型 context window 表 |
| `tool_registry.py` | 工具注册中心、OpenAI function-calling schema 生成 |
| `engine_tools.py` | 12 个 VSCode 对齐工具 + `engine` 聚合入口 |
| `chat_state.py` | 对话管理、tool 循环、@-mention、Agent Mode、自动压缩（90% context window 阈值） |
| `app.py` | pywebview 启动器 + `AIEditorAPI`（60+ JS-callable 方法） |
| `agents.py` | 自定义 Agent 系统：5 个内置 + `.sao/agents/*.json` 三 scope 加载 |
| `workflows.py` | Workflow 引擎：3 个内置 + 链式 LLM 调用 + `{{var}}` 变量插值 |
| `scopes.py` | 三层 scope（System/Workspace/Plugin）+ 三模式（Chat/Edit/Agent）+ 权限管理 |
| `chat_providers.py` | 右侧边栏 Chat Provider 动态注册（CHAT / Claude Code / Codex / 插件自定义） |
| `mcp_client.py` | MCP 客户端：stdio + HTTP/SSE + 内部 Python 注册 |
| `extensions.py` | VSCode Marketplace API 客户端 + VSIX tool 加载 |
| `history.py` | 对话历史分 scope 持久化（workspace / system 两级目录） |
| `selftest.py` | 113 项自测套件 |

## 布局（VSCode 对齐）

```
┌─────────┬──────────┬─────────────────┬──┬──────────────────┐
│ Activity│ Left     │  Editor         │⟷│ Right Sidebar    │
│ Bar     │ Sidebar  │  Tab bar        │  │ CHAT│CC│CODEX│..│
│         │          │  Code editor    │  │ ┌──────────────┐ │
│ 💬🔧🧩 │ Model    │                 │  │ │ Chat msgs    │ │
│ 📋📝🤖│ Actions  │  ──────────     │  │ │              │ │
│         │ Agents   │  Terminal/Output│  │ ├──────────────┤ │
│ 🌓⚙   │          │                 │  │ │ Input + tools│ │
├─────────┴──────────┴─────────────────┴──┴──────────────────┤
│ Status: Ready │ Ln 1 │ UTF-8 │ ✏️ Edit │ gpt-4o │ 0 tokens│
└────────────────────────────────────────────────────────────┘
```

- Chat 面板在**右侧边栏**（VSCode Claude Code / Copilot 布局）
- 底部面板只有 Terminal / Output
- 右侧边栏宽度可拖拽（260-600px）
- Provider tab 按 API key 可用性**动态出现**

## 三层 Scope 体系

| Scope | 路径 | 加载内容 |
|-------|------|---------|
| **System** | `~/.sao/` | 全局，跨所有项目 |
| **Workspace** | `<BASE_DIR>/.sao/` | 当前项目级 |
| **Plugin** | `plugins/<id>/.sao/` | 每个插件独立工作区 |

每个 scope 下可以有：
- `instructions.md` + `instructions/*.md` — 自定义指令
- `agents/*.json` — 自定义 Agent
- `workflows/*.json` — 自定义 Workflow
- `chat_history/` — 对话历史

合并策略：**全部加载，后者按 ID 覆盖前者**。

## 三模式（VSCode 对齐）

| 模式 | 说明 | 工具权限 |
|------|------|---------|
| **Chat** | 纯对话 | 读写/终端 disabled，只留 engine/askQuestion |
| **Edit** | 可读可编辑 | 读 allowed，写/终端 confirm |
| **Agent** | 全自主 | 全部 allowed |

状态栏点击循环切换。每个 tool 可独立覆盖权限（allowed/confirm/disabled）。

## Endpoint 全量配置

所有参数均可在 Settings UI 自定义：

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `provider` | str | openai | 提供商（openai/anthropic/deepseek/ollama/custom） |
| `api_key` | str | | API Key |
| `base_url` | str | | 自定义端点（空=默认） |
| `model` | str | | 模型名 |
| `temperature` | float | 0.7 | 采样温度 |
| `top_p` | float | 1.0 | 核采样 |
| `frequency_penalty` | float | 0.0 | 频率惩罚 |
| `presence_penalty` | float | 0.0 | 存在惩罚 |
| `max_tokens` | int | 4096 | 最大输出 token |
| `stop` | list | [] | 停止序列 |
| `max_input_tokens` | int | 0 | 覆盖模型默认输入窗口（0=用内置表） |
| `max_output_tokens` | int | 0 | 覆盖模型默认输出窗口 |
| `timeout` | int | 180 | HTTP 连接超时（秒） |
| `extra_headers` | dict | {} | 自定义 HTTP 头 |
| `extra_body` | dict | {} | 任意额外请求参数（如 logprobs） |

Settings UI 分三区：
- **Endpoint** — Provider、Key、Base URL
- **Provider Keys** — 分别填 OpenAI/Anthropic/DeepSeek（CC/Codex tab 按 key 自动出现）
- **Advanced Sampling & Connection** — 折叠区，top_p/penalties/stop/timeout/headers/body

## 模型 Context Window

内置 20+ 模型的 context window 表（`MODEL_CONTEXT_WINDOWS`）：

| 模型 | Max Input | Max Output |
|------|-----------|------------|
| gpt-4o | 128K | 16K |
| o3/o4-mini | 200K | 100K |
| claude-sonnet-4 | 200K | 16K |
| deepseek-chat | 64K | 8K |
| llama3.1 | 128K | 4K |

用户可在 Settings → Custom Models 区**添加/覆盖任何模型**：
```
save_custom_model("my-local-model", max_input=32000, max_output=4096)
```
覆盖存入 `settings.json → ai_editor.custom_models`。

## 自动压缩

对齐 VSCode Copilot 的 compaction 策略：
- 阈值 = `floor(model_max_input * 0.9)`
- 每轮 LLM 调用前估算 total tokens
- 超阈值时：动态计算保留多少条最近消息（填到 70% 以内）
- 旧消息压缩为一条 `[Compacted: N messages, Topics: ...]` 摘要
- 自定义 `max_input_tokens` 覆盖模型默认值

## Chat Provider 动态注册

右侧边栏 tab 由 provider 注册驱动：

| Provider | 检测条件 | 系统行为 |
|----------|---------|---------|
| **CHAT** | 始终存在 | 多模型，用户配置 |
| **Claude Code** | Anthropic key 已配置 或 `claude` CLI 可用 | 自动 Agent Mode |
| **Codex** | OpenAI key 已配置 | 自动 Agent Mode |
| **插件自定义** | 插件调用 `register_chat_provider()` | 按插件配置 |

每个 provider 有**独立的 ChatController + LLMEngine**，会话隔离。

### 插件注册 Provider

```python
# 在 plugin.py 中:
ctx.ai_editor.register_chat_provider({
    "id": "my-bot",
    "name": "My Bot",
    "icon": "🤖",
    "provider_type": "openai",
    "model": "gpt-4o-mini",
    "system_prompt": "You are a helpful assistant for my game.",
    "auto_agent": False,
})
```

## 自定义 Agent

5 个内置 Agent + 用户自定义（`.sao/agents/*.json`）：

| Agent | 用途 |
|-------|------|
| code-reviewer | 审查代码（bug/安全/性能/风格） |
| explainer | 解释代码逻辑和模式 |
| debugger | 系统化诊断和修复 |
| optimizer | 性能瓶颈分析 |
| documenter | 生成文档 |

激活方式：
- 侧边栏点击 Agent
- 聊天输入 `@code-reviewer 检查这个函数`
- LLM 调用 `engine(action="invoke_agent", agent_id="code-reviewer", message="...")`

Agent 可保存到任意 scope（System/Workspace/Plugin）。

## Workflow 引擎

3 个内置 Workflow + 用户自定义：

| Workflow | 步骤 |
|----------|------|
| review-and-fix | Code Reviewer 审查 → 生成修复 |
| explain-and-improve | Explainer 分析 → Optimizer 改进 |
| debug-trace | Debugger 诊断 → 生成修复 |

每个步骤可指定不同 Agent，支持 `{{var}}` 变量插值。LLM 可调用 `engine(action="run_workflow", workflow_id="review-and-fix", input="...")`。

自定义 Workflow JSON：
```json
{
  "id": "my-pipeline",
  "name": "My Pipeline",
  "steps": [
    {"agent": "code-reviewer", "prompt": "Review: {{input}}", "output_var": "review", "label": "Reviewing"},
    {"agent": "default", "prompt": "Fix issues:\n{{review}}", "output_var": "fix", "label": "Fixing"}
  ]
}
```

## Custom Instructions

三种来源，全部追加到 system prompt：
1. **Settings** — `ai_editor.user_instructions`（全局持久）
2. **项目** — `.sao/instructions.md`
3. **多文件** — `.sao/instructions/*.md`（按文件名排序）

跨三层 scope 全部收集。

## LLM 通讯协议

### OpenAI Chat Completions（默认）

适用于 OpenAI、DeepSeek、Ollama、vLLM 等所有兼容端点。全量采样参数（top_p/penalties/stop）+ extra_body 均传入请求体。

### Anthropic Messages API（原生）

provider=anthropic 时自动切换。支持 thinking block、tool_use、stop_sequences、extra_body。

## 工具系统

12 个工具，5 个分类（file/terminal/interaction/editor/engine）。模式权限控制哪些工具对 LLM 可见。

`engine` 聚合入口的 action 包括：
- 平台：`system_info`、`plugins`、`settings_get/set`、`memory_status`、`eval`、`exec`
- Agent：`list_agents`、`invoke_agent`
- Workflow：`list_workflows`、`run_workflow`
- 插件动态注册的 action

## MCP 集成

三种传输：stdio / HTTP+SSE / internal Python。配置来源：settings → workspace mcp.json → ~/.sao/mcp.json → plugin manifest。

## 对话历史

分 scope 持久化：
- Workspace: `<BASE_DIR>/.sao/chat_history/`
- System: `~/.sao/chat_history/`
- 兼容旧 `ai_editor_history/` 目录
- `list_conversations(scope="all")` 跨 scope 搜索

## 验证

```bash
python -m ai_editor.selftest     # 113 项自测
python -m ai_editor.app           # 独立启动 GUI
```
