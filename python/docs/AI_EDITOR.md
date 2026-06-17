# AI Editor 使用指南

SAO ACT UI 内置的 AI 编辑器是一个独立窗口，提供多模型 LLM 对话、代码编辑、自定义 Agent 与 Workflow、插件扩展，以及 dark/light 双主题。

---

## 打开方式

- **菜单**：工具 → AI Editor (LLM)
- **命令行**：`python -m ai_editor.app`（独立启动，不需要主界面运行）

## 窗口布局

```
┌─────────┬──────────┬─────────────────┬──┬──────────────────┐
│ 活动栏  │ 左侧边栏 │ 编辑器区域      │⟷│ 右侧边栏         │
│         │          │ 标签页栏        │  │ CHAT│CC│CODEX│.. │
│ 聊天    │ 模型选择 │ 代码编辑器      │  │ ┌──────────────┐ │
│ 工具    │ 操作列表 │                 │  │ │ 对话消息     │ │
│ 插件    │ Agent 列 │ ─────────       │  │ │              │ │
│ 历史    │          │ 终端 / 输出     │  │ ├──────────────┤ │
│         │          │                 │  │ │ 输入框+工具  │ │
│ 主题 ⚙ │          │                 │  │ └──────────────┘ │
├─────────┴──────────┴─────────────────┴──┴──────────────────┤
│ 状态: Ready │ Ln 1 │ UTF-8 │ 编辑模式 │ gpt-4o │ 0 tokens │
└────────────────────────────────────────────────────────────┘
```

**关键区域说明：**

- **左侧活动栏**：图标按钮，切换左侧边栏的面板（聊天、工具、插件、历史），底部是主题切换和设置入口。
- **左侧边栏**：显示当前模型选择、可用操作、Agent 列表。
- **编辑器**：中央大区域，上方标签页栏，下方可折叠终端/输出面板。
- **右侧边栏**：对话面板。顶部是 Provider 标签页（CHAT、Claude Code、Codex 等），按已配置的 API Key 动态出现。宽度可拖拽（260-600px）。
- **状态栏**：底部一行，显示当前模式、模型、token 用量。点击模式区域可循环切换。

---

## 设置配置

点击活动栏底部的 ⚙ 齿轮图标打开设置面板。

### Endpoint（端点配置）

| 设置项 | 说明 |
|--------|------|
| `provider` | 提供商：openai / anthropic / deepseek / ollama / custom |
| `api_key` | 当前提供商的 API Key |
| `base_url` | 自定义端点地址（留空使用官方默认地址） |
| `model` | 模型名称 |

### Provider Keys（多提供商密钥）

分别填入 OpenAI、Anthropic、DeepSeek 的 API Key。右侧边栏的 Claude Code 和 Codex 标签页根据对应 Key 是否已配置自动出现或隐藏。

### Advanced Sampling & Connection（高级参数，折叠区）

| 设置项 | 类型 | 默认 | 说明 |
|--------|------|------|------|
| `temperature` | float | 0.7 | 采样温度，越高越随机 |
| `top_p` | float | 1.0 | 核采样 |
| `frequency_penalty` | float | 0.0 | 频率惩罚 |
| `presence_penalty` | float | 0.0 | 存在惩罚 |
| `max_tokens` | int | 4096 | 单次回复最大输出 token |
| `stop` | list | [] | 停止序列 |
| `max_input_tokens` | int | 0 | 覆盖模型默认输入窗口（0 = 使用内置表） |
| `max_output_tokens` | int | 0 | 覆盖模型默认输出窗口 |
| `timeout` | int | 180 | HTTP 超时（秒） |
| `extra_headers` | dict | {} | 自定义 HTTP 请求头 |
| `extra_body` | dict | {} | 额外请求体参数 |

### Custom Models（自定义模型）

在 Settings → Custom Models 区域可以添加或覆盖任何模型的 context window 大小。编辑器内置了常见模型的窗口大小表，但如果你使用私有部署或新模型，可以在此手动设定。自定义模型保存在 `settings.json → ai_editor.custom_models`。

---

## 三种工作模式

状态栏显示当前模式，点击可循环切换：

| 模式 | 说明 | LLM 可用工具范围 |
|------|------|------------------|
| **Chat** | 纯对话 | 只有问答，不能读写文件或执行命令 |
| **Edit** | 可读可编辑 | 可以读文件，写文件和终端操作需要确认 |
| **Agent** | 全自主 | 所有工具自动可用，无需确认 |

每个工具还可以单独设置权限覆盖（allowed / confirm / disabled）。

---

## 三层 Scope 体系

配置、Agent、Workflow、指令文件分三层存放，后者按 ID 覆盖前者：

| Scope | 路径 | 作用范围 |
|-------|------|----------|
| **System** | `~/.sao/` | 全局，跨所有项目生效 |
| **Workspace** | `<项目根>/.sao/` | 当前项目 |
| **Plugin** | `plugins/<插件ID>/.sao/` | 单个插件的独立工作区 |

每个 scope 目录下可以放：

```
.sao/
├── instructions.md           # 自定义指令（追加到 system prompt）
├── instructions/             # 多文件指令（按文件名排序合并）
│   ├── 01-style.md
│   └── 02-rules.md
├── agents/                   # 自定义 Agent 定义
│   └── my-agent.json
├── workflows/                # 自定义 Workflow 定义
│   └── my-pipeline.json
└── chat_history/             # 对话历史（自动管理）
```

---

## 自定义指令

自定义指令会追加到每次 LLM 调用的 system prompt 中。三种来源：

1. **Settings UI** — 在设置面板的 System Prompt 文本框中填写，全局持久保存。
2. **项目级** — 在 `.sao/instructions.md` 中编写。
3. **多文件** — 在 `.sao/instructions/` 目录下放多个 `.md` 文件，按文件名排序合并。

三层 scope 的指令全部收集叠加。

---

## 自定义 Agent

AI Editor 内置了 5 个常用 Agent：

| Agent | 用途 |
|-------|------|
| code-reviewer | 审查代码（bug、安全、性能、风格） |
| explainer | 解释代码逻辑和设计模式 |
| debugger | 系统化诊断和修复问题 |
| optimizer | 分析性能瓶颈并给出优化建议 |
| documenter | 生成 API 文档、注释、指南 |

### 使用方式

- **侧边栏**：在左侧边栏的 Agent 列表中点击激活
- **@提及**：在聊天输入框中输入 `@agent-id 你的问题`，例如 `@code-reviewer 检查这个函数`
- **LLM 工具调用**：`engine(action="invoke_agent", agent_id="code-reviewer", message="...")`

### 创建自定义 Agent

在任意 scope 的 `.sao/agents/` 目录下新建 JSON 文件，例如 `.sao/agents/my-agent.json`：

```json
{
  "id": "game-analyst",
  "name": "Game Analyst",
  "description": "分析游戏数据和战斗日志",
  "system_prompt": "你是一个游戏数据分析专家。擅长解读战斗日志、DPS统计、装备属性，给出优化建议。",
  "icon": "🎮",
  "tools": ["readFile", "engine"],
  "model": "gpt-4o",
  "when_to_use": "分析游戏数据、战斗日志、装备搭配"
}
```

**字段说明：**

| 字段 | 必填 | 说明 |
|------|------|------|
| `id` | 是 | 唯一标识，用于 @提及和 API 调用 |
| `name` | 是 | 显示名称 |
| `description` | 否 | 简短描述，显示在侧边栏 |
| `system_prompt` | 是 | Agent 的系统提示词 |
| `icon` | 否 | Emoji 图标，默认为通用图标 |
| `tools` | 否 | 允许使用的工具列表（空 = 全部） |
| `model` | 否 | 指定模型（空 = 使用当前选择的模型） |
| `when_to_use` | 否 | 描述适用场景，帮助 LLM 自动选择 |

Agent 保存到不同 scope 的效果：
- `~/.sao/agents/` — 所有项目都能用
- `<项目根>/.sao/agents/` — 只在当前项目可用
- `plugins/<id>/.sao/agents/` — 只在该插件的工作区可用

同 ID 的 Agent，Workspace 覆盖 System，Plugin 覆盖 Workspace。

---

## 自定义 Workflow

Workflow 是多步骤的链式 LLM 调用，每一步可以使用不同的 Agent，上一步的输出可以通过变量传递给下一步。

内置了 3 个 Workflow：

| Workflow | 步骤 |
|----------|------|
| review-and-fix | Code Reviewer 审查 → 生成修复代码 |
| explain-and-improve | Explainer 分析 → Optimizer 改进 |
| debug-trace | Debugger 诊断 → 生成修复方案 |

### 使用方式

- **侧边栏**：在 Workflow 列表中点击运行
- **LLM 工具调用**：`engine(action="run_workflow", workflow_id="review-and-fix", input="...")`

### 创建自定义 Workflow

在 `.sao/workflows/` 目录下新建 JSON 文件，例如 `.sao/workflows/my-pipeline.json`：

```json
{
  "id": "analyze-and-report",
  "name": "分析并生成报告",
  "description": "先用分析 Agent 审查代码，再汇总成格式化报告",
  "icon": "📊",
  "steps": [
    {
      "agent": "code-reviewer",
      "prompt": "审查以下代码，列出所有问题：\n\n{{input}}",
      "output_var": "review_result",
      "label": "审查中..."
    },
    {
      "agent": "documenter",
      "prompt": "根据以下审查结果生成一份格式化报告，包含问题分类和优先级：\n\n{{review_result}}",
      "output_var": "report",
      "label": "生成报告..."
    }
  ]
}
```

**步骤字段说明：**

| 字段 | 说明 |
|------|------|
| `agent` | 使用哪个 Agent 执行这一步（`"default"` = 不指定 Agent） |
| `prompt` | 发给 LLM 的提示词，支持 `{{变量名}}` 插值 |
| `output_var` | 把这一步的输出存到这个变量名，后续步骤可以用 `{{变量名}}` 引用 |
| `label` | 执行时显示的进度文字 |

初始输入通过 `{{input}}` 变量获取。

---

## Chat Provider（对话提供商）

右侧边栏的标签页由 Provider 驱动：

| 标签页 | 出现条件 | 说明 |
|--------|----------|------|
| **CHAT** | 始终存在 | 通用对话，使用你在 Settings 中配置的模型 |
| **Claude Code** | 已配置 Anthropic API Key 或本机有 `claude` CLI | 自动进入 Agent 模式 |
| **Codex** | 已配置 OpenAI API Key | 自动进入 Agent 模式 |
| **插件自定义** | 插件调用 `register_chat_provider()` 注册 | 按插件配置运行 |

每个 Provider 标签页有独立的对话会话和 LLM 引擎实例，互不干扰。

---

## 对话历史

对话自动保存，分 scope 存放：
- **Workspace 历史**：`<项目根>/.sao/chat_history/`
- **System 历史**：`~/.sao/chat_history/`

可以在左侧边栏的历史面板中浏览和恢复之前的对话。

---

## MCP 集成

AI Editor 支持连接 MCP 服务器，扩展 LLM 可用的工具。配置来源按优先级：

1. Settings UI 中的 MCP 配置
2. 项目根目录 `mcp.json`
3. `~/.sao/mcp.json`
4. 插件 manifest 中声明的 MCP 服务器

支持 stdio、HTTP+SSE、内部 Python 三种传输方式。

---

# 插件开发者指南

以下内容面向想在 AI Editor 中添加自定义对话标签页或扩展功能的插件开发者。

## 注册 Chat Provider

在插件的 `plugin.py` 中通过 `ctx.ai_editor.register_chat_provider()` 注册一个对话标签页：

```python
def on_enable(ctx):
    ctx.ai_editor.register_chat_provider({
        "id": "my-bot",
        "name": "My Bot",
        "icon": "🤖",
        "provider_type": "openai",      # openai / anthropic / custom
        "model": "gpt-4o-mini",
        "base_url": "",                  # 留空使用默认端点
        "system_prompt": "你是一个游戏辅助助手，专注于回答战斗相关问题。",
        "auto_agent": False,             # 是否默认开启 Agent 模式
    })
```

**ChatProviderDef 全部字段：**

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `id` | str | (必填) | 唯一标识 |
| `name` | str | (必填) | 标签页显示名称 |
| `icon` | str | 💬 | Emoji 图标 |
| `provider_type` | str | openai | LLM 提供商 |
| `api_key` | str | "" | API Key（不填则使用用户在 Settings 中配置的 Key） |
| `base_url` | str | "" | 自定义端点 |
| `model` | str | "" | 模型名 |
| `system_prompt` | str | "" | 系统提示词 |
| `auto_agent` | bool | False | 打开时是否自动切到 Agent 模式 |

注册后，标签页立即出现在右侧边栏中。

## 通过 Scope 分发 Agent 和 Workflow

插件可以在自己的 `.sao/` 目录下放置 Agent 和 Workflow 定义文件，它们会自动被加载：

```
plugins/my-plugin/
├── plugin.py
└── .sao/
    ├── agents/
    │   └── my-plugin-agent.json
    ├── workflows/
    │   └── my-plugin-workflow.json
    └── instructions.md           # 插件级自定义指令
```

这些定义只在该插件的工作区生效，不会影响其他插件或全局配置。

## engine 工具扩展

插件可以通过 engine 聚合入口注册自定义 action，扩展 LLM 在对话中可调用的能力。engine 已有的 action 包括：

- **平台相关**：`system_info`、`plugins`、`settings_get`、`settings_set`、`memory_status`、`eval`、`exec`
- **Agent 相关**：`list_agents`、`invoke_agent`
- **Workflow 相关**：`list_workflows`、`run_workflow`
- **插件动态注册的 action**
