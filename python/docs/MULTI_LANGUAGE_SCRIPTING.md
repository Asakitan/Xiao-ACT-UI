# 多语言插件脚本运行时

除 Python 外，插件还可以用 **Lua**、**C#**、**AngelScript**、**Emma** 编写。所有语言的插件都能调用完整的平台 SDK（46+ 方法），与 Python 插件功能完全一致。

> 本文档是 [PLUGIN_SDK.md](PLUGIN_SDK.md) 的补充。SDK API 的完整说明请参考那篇文档，本文只讲各语言的写法差异和环境配置。

---

## 支持矩阵

| 语言 | 入口扩展名 | 运行时 | 外部依赖 | 适合场景 |
|------|:----------:|--------|:--------:|----------|
| Python | `.py` | 原生 | 无 | 完整能力，性能关键路径可 Cython |
| Lua | `.lua` | lupa (LuaJIT) | `pip install lupa` | 热更新脚本、配置驱动逻辑、模组社区 |
| C# | `.cs` / `.dll` | pythonnet + .NET CLR | `pip install pythonnet` + .NET SDK | 强类型、Unity 风格、复用 .NET 库 |
| AngelScript | `.as` | 内置纯 Python 解释器 | **无** | 类 C 语法、游戏脚本风格、零配置 |
| Emma | `.emma` | 内置纯 Python 解释器 | **无** | 类 Python 语法、轻量、零配置 |

---

## 快速开始

### 第一步：创建插件目录

```
my_lua_plugin/
├── plugin.json
└── plugin.lua
```

### 第二步：写 plugin.json

在常规字段基础上，加一个 `language` 字段（也可以省略，平台会从 `entry` 扩展名自动推断）：

```json
{
  "id": "my_lua_plugin",
  "name": "我的 Lua 插件",
  "version": "1.0.0",
  "entry": "plugin.lua",
  "language": "lua",
  "enabled": true,
  "capabilities": [
    {"id": "ui_panels", "title": "Lua 面板"}
  ]
}
```

`language` 支持的值和别名：

| 值 | 别名 |
|----|------|
| `python` | `py` |
| `lua` | — |
| `csharp` | `cs`、`c#`、`dotnet`、`.net` |
| `angelscript` | `as`、`angel` |
| `emma` | — |

### 多语言 manifest

脚本插件和 Python 插件使用同一套 manifest 本地化字段。基础 `name`、`description`、`sao_menu`、`settings_schema` 建议保留默认中文；额外语言写在 `locales`（也兼容 `i18n`、`translations`）里：

```json
{
  "id": "my_lua_plugin",
  "name": "我的 Lua 插件",
  "description": "Lua 示例插件",
  "entry": "plugin.lua",
  "language": "lua",
  "sao_menu": {
    "name": "Lua 工具",
    "icon_text": "▣",
    "script_label": "Lua 工具",
    "actions": [
      {"id": "script.state", "label": "读取状态"}
    ]
  },
  "settings_schema": {
    "overlay_enabled": {
      "type": "boolean",
      "default": false,
      "description": "是否显示叠加层"
    },
    "detail_mode": {
      "type": "string",
      "default": "seconds",
      "enum": ["seconds", "minute"],
      "enum_labels": {
        "seconds": "秒级刷新",
        "minute": "分钟刷新"
      },
      "description": "刷新细节"
    }
  },
  "locales": {
    "en-US": {
      "name": "My Lua Plugin",
      "description": "Lua sample plugin",
      "sao_menu": {
        "name": "Lua Tool",
        "script_label": "Lua Tool",
        "actions": {
          "script.state": {"label": "Read state"}
        }
      },
      "settings_schema": {
        "overlay_enabled": {
          "description": "Show the overlay"
        },
        "detail_mode": {
          "description": "Refresh detail",
          "enum_labels": {
            "seconds": "Seconds",
            "minute": "Minute"
          }
        }
      }
    }
  }
}
```

本地化覆盖只改显示文本，不会改变 `enabled`、设置 `default/type/enum`、设置选项 `value`、菜单 `priority/surface`、权限、capability id 或 action id。`settings_schema` 里已有 `options`/`choices` 的选项文案可按 `id`/`value` 覆盖，`enum_labels`/`value_labels` 可按已有枚举值覆盖。这样插件管理器、SAO popup、settings 面板可以即时换语言，同时不会因为翻译包改变脚本加载和运行行为。

### 第三步：写入口脚本

每种语言的入口脚本都要定义相同的四个生命周期函数（只有 `on_load` 是必须的）：

| 函数 | 参数 | 调用时机 |
|------|------|----------|
| `on_load(ctx)` | 平台 SDK 上下文 | 插件加载 |
| `on_enable()` | 无 | 启用后 |
| `on_disable()` | 无 | 禁用时 |
| `on_unload()` | 无 | 卸载时 |

### 第四步：放入 plugins/ 或 user_plugins/

和 Python 插件完全一样。也可以打成 `.zip` 通过一键导入安装。

---

## Lua 插件

### 环境配置

```bash
pip install lupa
```

lupa 内嵌 LuaJIT，安装后即用，无需额外配置。

### 语法

Lua 通过冒号语法 `:` 调用 `ctx` 上的方法（等价于 Python 的 `ctx.method()`）：

```lua
-- plugin.lua

local _ctx = nil
local count = 0

function on_load(ctx)
    _ctx = ctx
    ctx:log("Lua 插件已加载!")

    ctx:set_defaults({greeting = "你好"})

    ctx:register_ui_panel("lua_demo",
        {title = "Lua Demo", description = "Lua 语言插件示例"},
        render_panel,
        on_panel_action
    )

    -- 订阅伤害事件
    ctx:subscribe("damage", function(event)
        ctx:log("伤害: " .. tostring(event.amount or 0))
    end)

    -- 注册快捷键
    ctx:register_hotkey("toggle", function()
        ctx:toast("快捷键触发!")
    end, "CTRL+F8", "Lua 切换")
end

function render_panel(payload)
    local greeting = _ctx:get_setting("greeting", "你好")
    return _ctx.ui.panel("Lua Plugin", {
        _ctx.ui.section("状态", {
            _ctx.ui.kv("语言", "Lua (LuaJIT)"),
            _ctx.ui.kv("问候语", greeting),
            _ctx.ui.kv("点击数", tostring(count)),
        }),
        _ctx.ui.section("操作", {
            _ctx.ui.row({
                _ctx.ui.button("问候", "greet", "primary"),
                _ctx.ui.button("计数", "count"),
            }),
        }),
    })
end

function on_panel_action(action_id, payload)
    if action_id == "greet" then
        local g = _ctx:get_setting("greeting", "你好")
        _ctx:notify("Lua", g .. "!", 3.0)
    elseif action_id == "count" then
        count = count + 1
        _ctx:request_redraw("lua_demo")
    end
    return {ok = true}
end

function on_unload()
    _ctx:log("Lua 插件已卸载")
end
```

### Lua 特性速查

| Python | Lua |
|--------|-----|
| `ctx.method()` | `ctx:method()` |
| `dict` | `table`（自动转换） |
| `list` | 连续整数键 table（自动转换） |
| `True` / `False` / `None` | `true` / `false` / `nil` |
| `f"string {var}"` | `"string " .. tostring(var)` |
| `lambda: expr` | `function() return expr end` |

lupa 自动在 Lua table 和 Python dict/list 之间双向转换。`ctx.ui` 上的方法（`.panel()`、`.text()` 等）用点号调用，因为它们是静态方法而非实例方法。

---

## C# 插件

### 环境配置

```bash
pip install pythonnet
```

还需要 .NET 运行时（.NET 6.0+）。如果 entry 是 `.cs` 源码文件，编译时还需要 .NET SDK（`dotnet` 命令或 `csc.exe` 在 PATH 上）。

### 两种模式

**源码模式**（`.cs`）：平台自动编译为临时 DLL，适合开发期。

**程序集模式**（`.dll`）：直接加载预编译的 .NET 程序集，适合发布分发。

```json
{
  "entry": "plugin.cs",
  "language": "csharp"
}
```

或者预编译：

```json
{
  "entry": "plugin.dll",
  "language": "csharp"
}
```

### 语法

C# 插件定义一个 `public class Plugin`（名称可以是 `Plugin`、`PluginEntry`、或插件 ID）。`ctx` 作为 `dynamic` 对象传入，可以直接调用平台全部 SDK 方法。

```csharp
// plugin.cs
using System;
using System.Collections.Generic;

public class Plugin
{
    private dynamic _ctx;
    private int _clickCount = 0;

    // --- 生命周期 ---
    public void OnLoad(dynamic ctx)
    {
        _ctx = ctx;
        ctx.log("C# 插件已加载!");

        ctx.set_defaults(new Dictionary<string, object> {
            {"greeting", "你好"}
        });

        ctx.register_ui_panel("cs_demo",
            new Dictionary<string, object> {
                {"title", "C# Demo"},
                {"description", "C# 语言插件示例"}
            },
            new Func<object, object>(Render),
            new Func<string, object, object>(OnAction)
        );
    }

    public void OnEnable()  { _ctx?.log("C# 启用"); }
    public void OnDisable() { _ctx?.log("C# 禁用"); }
    public void OnUnload()  { _ctx?.log("C# 卸载"); }

    // --- 面板渲染 ---
    public object Render(object payload)
    {
        string greeting = (string)(_ctx.get_setting("greeting", "你好") ?? "你好");
        return _ctx.ui.panel("C# Plugin", new object[] {
            _ctx.ui.section("状态", new object[] {
                _ctx.ui.kv("语言", "C# (.NET)"),
                _ctx.ui.kv("问候语", greeting),
                _ctx.ui.kv("点击数", _clickCount.ToString()),
            }),
            _ctx.ui.row(new object[] {
                _ctx.ui.button("问候", "greet", "primary"),
                _ctx.ui.button("计数", "count"),
            }),
        });
    }

    // --- 按钮动作 ---
    public object OnAction(string actionId, object payload)
    {
        if (actionId == "greet")
        {
            string g = (string)(_ctx.get_setting("greeting", "你好") ?? "你好");
            _ctx.notify("C#", g + "!", 3.0);
        }
        else if (actionId == "count")
        {
            _clickCount++;
            _ctx.request_redraw("cs_demo");
        }
        return new Dictionary<string, object> { {"ok", true} };
    }
}
```

### C# 注意事项

- 回调函数用 `Func<>` 委托包装（pythonnet 需要显式委托类型）
- 字典用 `Dictionary<string, object>`，列表用 `object[]`
- `ctx` 是 `dynamic` 类型，方法名与 Python SDK 一致（小写下划线风格）
- 生命周期方法支持 PascalCase（`OnLoad`）和 snake_case（`on_load`）两种写法
- 静态方法和实例方法都支持；如果所有 `On*` 方法都是 static，平台不会实例化类
- 引用外部 .NET DLL：放在插件目录的 `refs/` 文件夹下，编译时自动添加引用

---

## AngelScript 插件

### 环境配置

**无需安装任何依赖**。使用内置的纯 Python AngelScript 子集解释器。

### 语法

AngelScript 是类 C/C++ 风格的脚本语言，常见于游戏引擎（如 Unreal）。平台实现了一个实用子集，足以覆盖插件开发所需的全部场景。

```as
// plugin.as

PluginContext@ ctx;
int click_count = 0;

void on_load(PluginContext@ c)
{
    @ctx = c;
    ctx.log("AngelScript 插件已加载!");
    ctx.set_defaults({"greeting": "你好"});

    ctx.register_ui_panel("as_demo",
        {"title": "AS Demo", "description": "AngelScript 插件示例"},
        @render_panel, @on_panel_action);
}

dictionary@ render_panel(dictionary@ payload)
{
    string greeting = ctx.get_setting("greeting", "你好");
    return ctx.ui_panel("AngelScript Plugin", {
        ctx.ui_section("状态", {
            ctx.ui_kv("语言", "AngelScript (内置)"),
            ctx.ui_kv("问候语", greeting),
            ctx.ui_kv("点击数", tostring(click_count))
        }),
        ctx.ui_section("操作", {
            ctx.ui_row({
                ctx.ui_button("问候", "greet", "primary"),
                ctx.ui_button("计数", "count")
            })
        })
    });
}

dictionary@ on_panel_action(string action_id, dictionary@ payload)
{
    if (action_id == "greet")
    {
        string g = ctx.get_setting("greeting", "你好");
        ctx.notify("AS", g + "!", 3.0);
    }
    if (action_id == "count")
    {
        click_count = click_count + 1;
        ctx.request_redraw("as_demo");
    }
    return {"ok": true};
}

void on_enable()  { ctx.log("AS 启用"); }
void on_disable() { ctx.log("AS 禁用"); }
void on_unload()  { ctx.log("AS 卸载"); }
```

### AngelScript 特性速查

| 特性 | 写法 |
|------|------|
| 变量声明 | `int x = 42;` / `string name = "hello";` |
| 函数定义 | `void func(int a, string b) { ... }` |
| 句柄引用 | `PluginContext@ ctx;` / `@ctx = c;` |
| 函数引用 | `@render_panel`（传回调时用） |
| 字典字面量 | `{"key": "value", "num": 42}` |
| 字符串拼接 | `"hello " + "world"` |
| 类型转换 | `tostring(42)` / `toint("42")` / `tofloat("3.14")` |
| 条件 | `if (cond) { ... } else { ... }` |
| 循环 | `while (cond) { ... }` |
| 注释 | `// 单行` / `/* 多行 */` |

### AngelScript UI 快捷方法

AngelScript 的 `ctx` 对象额外提供了扁平化 UI 方法（避免 `ctx.ui.xxx` 的链式访问在子集解释器中出问题）：

```as
ctx.ui_panel(title, children)          // = ctx.ui.panel()
ctx.ui_section(title, children)        // = ctx.ui.section()
ctx.ui_text(text, style, align)        // = ctx.ui.text()
ctx.ui_kv(label, value, style)         // = ctx.ui.kv()
ctx.ui_button(label, action, style)    // = ctx.ui.button()
ctx.ui_badge(text, style)              // = ctx.ui.badge()
ctx.ui_bar(label, pct, color, caption) // = ctx.ui.bar()
ctx.ui_row(children, align)            // = ctx.ui.row()
ctx.ui_divider()                       // = ctx.ui.divider()
ctx.ui_table(columns, rows, ...)       // = ctx.ui.table()
```

### 解释器限制

内置解释器覆盖了实用子集，以下特性**不支持**：

- `class` / `interface` 定义
- 泛型 / 模板 (`array<int>`)
- `try` / `catch` / `throw`
- `switch` / `case`
- `enum` 定义
- 运算符重载
- 命名空间 (`namespace`)

这些限制不影响插件开发——生命周期 hook、SDK 调用、控制流、递归、字典/数组操作全部正常工作。

---

## Emma 插件

### 环境配置

**无需安装任何依赖**。Emma 是平台内置的轻量级脚本语言。

### 设计哲学

Emma 的语法接近 Python，但用 `end` 代替缩进来标记块边界。这使得解析更可靠，同时保持了 Python 使用者的熟悉感。

### 语法

```emma
-- plugin.emma

let click_count = 0
let timer_token = ""
let timer_running = false

fn on_load(ctx)
    ctx.log("Emma 插件已加载!")
    ctx.set_defaults({greeting: "你好", auto_count: false})

    ctx.register_ui_panel("emma_demo",
        {title: "Emma Demo", description: "Emma 语言插件示例"},
        render_panel, on_panel_action)

    -- 订阅事件
    ctx.subscribe("damage", fn(event)
        ctx.log("Emma 收到伤害事件")
    end)
end

fn render_panel(payload)
    let greeting = ctx.get_setting("greeting", "你好")
    let status_text = "停止"
    if timer_running
        status_text = "运行中"
    end

    return ctx.ui.panel("Emma Plugin", [
        ctx.ui.section("状态", [
            ctx.ui.kv("语言", "Emma (内置)"),
            ctx.ui.kv("问候语", greeting),
            ctx.ui.kv("点击数", tostring(click_count)),
            ctx.ui.kv("定时器", status_text),
        ]),
        ctx.ui.section("操作", [
            ctx.ui.row([
                ctx.ui.button("问候", "greet", "primary"),
                ctx.ui.button("计数", "count"),
                ctx.ui.button("定时器开关", "timer_toggle"),
            ]),
        ]),
    ])
end

fn on_panel_action(action_id, payload)
    if action_id == "greet"
        let g = ctx.get_setting("greeting", "你好")
        ctx.notify("Emma", g .. "!", 3.0)
    elif action_id == "count"
        click_count = click_count + 1
        ctx.request_redraw("emma_demo")
    elif action_id == "timer_toggle"
        if timer_running
            ctx.clear_timer(timer_token)
            timer_running = false
        else
            timer_token = ctx.set_interval(fn()
                click_count = click_count + 1
                ctx.request_redraw("emma_demo")
            end, 2.0)
            timer_running = true
        end
        ctx.request_redraw("emma_demo")
    end
    return {ok: true}
end

fn on_disable()
    if timer_running
        ctx.clear_timer(timer_token)
        timer_running = false
    end
end

fn on_unload()
    ctx.log("Emma 插件已卸载")
end
```

### Emma 语法参考

**变量**：

```emma
let name = "hello"          -- 声明
name = "world"              -- 赋值
let items = [1, 2, 3]       -- 数组
let config = {key: "value"} -- 字典
```

**函数**：

```emma
fn add(a, b)
    return a + b
end

-- 匿名函数（lambda）
let double = fn(x)
    return x * 2
end
```

**控制流**：

```emma
-- 条件
if x > 0
    ctx.log("正数")
elif x == 0
    ctx.log("零")
else
    ctx.log("负数")
end

-- 循环
while count < 10
    count = count + 1
end

-- 遍历
for item in items
    ctx.log(tostring(item))
end

for i in range(10)
    ctx.log(tostring(i))
end

-- 跳出
break
continue
```

**运算符**：

| 类型 | 运算符 |
|------|--------|
| 算术 | `+` `-` `*` `/` `%` |
| 比较 | `==` `!=` `<` `>` `<=` `>=` |
| 逻辑 | `and` `or` `not` |
| 字符串拼接 | `..` 或 `+` |
| 取否 | `!`（等价于 `not`） |

**数据类型**：

| 类型 | 字面量 |
|------|--------|
| 整数 | `42`、`0xFF` |
| 浮点 | `3.14`、`1e-3` |
| 字符串 | `"hello"` 或 `'hello'` |
| 布尔 | `true` / `false` |
| 空值 | `nil` |
| 数组 | `[1, 2, 3]` |
| 字典 | `{key: "value", num: 42}` |

**内置函数**：

```emma
str(42)              -- "42"
int("42")            -- 42
float("3.14")        -- 3.14
len([1, 2, 3])       -- 3
type(42)             -- "int"
tostring(x)          -- 转字符串
tonumber(x)          -- 转数字
range(10)            -- 0..9 迭代器
abs(-5)              -- 5
min(1, 2)            -- 1
max(1, 2)            -- 2
pairs({a: 1})        -- [("a", 1)] 键值对列表
ipairs([10, 20])     -- [(0, 10), (1, 20)] 索引列表
json_encode(obj)     -- JSON 序列化
json_decode(str)     -- JSON 反序列化
time()               -- 当前 Unix 时间戳
sleep(seconds)       -- 休眠
log(...)             -- 日志输出
```

---

## 通用注意事项

### 所有语言共用同一套 SDK

不管用什么语言，`ctx` 对象暴露的方法完全一致。差异只在调用语法：

| 操作 | Python | Lua | C# | AngelScript | Emma |
|------|--------|-----|----|-------------|------|
| 日志 | `ctx.log("msg")` | `ctx:log("msg")` | `ctx.log("msg")` | `ctx.log("msg");` | `ctx.log("msg")` |
| 设置 | `ctx.get_setting("k")` | `ctx:get_setting("k")` | `ctx.get_setting("k")` | `ctx.get_setting("k");` | `ctx.get_setting("k")` |
| 面板 | `ctx.register_ui_panel(...)` | `ctx:register_ui_panel(...)` | `ctx.register_ui_panel(...)` | `ctx.register_ui_panel(...);` | `ctx.register_ui_panel(...)` |
| 通知 | `ctx.notify(t, m)` | `ctx:notify(t, m)` | `ctx.notify(t, m)` | `ctx.notify(t, m);` | `ctx.notify(t, m)` |

### 类型转换

各运行时自动处理 Python ↔ 目标语言的类型转换：

| Python 类型 | Lua | C# | AngelScript | Emma |
|-------------|-----|----|-------------|------|
| `dict` | table | `Dictionary<string, object>` | dict literal | dict |
| `list` | table (连续键) | `object[]` / `List<T>` | array | list |
| `str` | string | `string` | string | string |
| `int` / `float` | number | `int` / `double` | int / float | int / float |
| `bool` | boolean | `bool` | bool (true/false) | bool (true/false) |
| `None` | nil | `null` | null/nil | nil |

### 回调函数

所有语言都可以向 SDK 传递回调函数（事件订阅、面板渲染、定时器等）。回调在 Python 进程中执行，异常被隔离不会影响其他插件。

### 错误处理

- 脚本语法错误在加载时抛出，插件不会进入运行状态
- 运行时异常被捕获并记录到插件日志
- 连续失败 3 次后自动禁用（与 Python 插件一致）

### 依赖管理

- Lua / C# 插件同样可以使用 `vendor/`、`libs/`、`requirements.txt` 机制管理 Python 侧依赖
- C# 插件的 .NET 引用 DLL 放在 `refs/` 目录下
- AngelScript / Emma 插件零依赖，开箱即用

### 打包分发

ZIP 打包方式与 Python 插件完全一致，只是入口文件扩展名不同：

```
my_emma_plugin.zip
└── my_emma_plugin/
    ├── plugin.json
    ├── plugin.emma
    └── assets/
```

---

## 运行时状态查询

在代码中查询各语言运行时的可用状态：

```python
from act_platform.scripting import list_runtimes, runtime_available

# 查看全部运行时
runtimes = list_runtimes()
# {'python': {'available': True, ...},
#  'lua': {'available': False, 'error': 'lupa not installed'},
#  'csharp': {'available': True, 'engine': 'pythonnet (.NET 8.0)'},
#  'angelscript': {'available': True, 'engine': 'AngelScript (pure-Python)'},
#  'emma': {'available': True, 'engine': 'Emma (built-in)'}}

# 检查单个语言
if runtime_available("lua"):
    print("Lua 可用")
```

`PluginManager.status()` 返回值中的 `script_runtimes` 字段包含同样的信息。

---

## 故障排查

### Lua 插件加载失败

1. 确认已安装 lupa：`pip install lupa`
2. 检查 Lua 语法（特别是 `end` 配对和 `:` vs `.` 调用）
3. 查看插件日志中的错误信息

### C# 插件编译失败

1. 确认 .NET SDK 已安装且 `dotnet` 在 PATH 上
2. 检查 C# 语法错误（编译器输出会记录在插件日志中）
3. 如果引用了外部 DLL，放在插件目录的 `refs/` 下
4. 也可以在本地预编译为 `.dll`，在 `plugin.json` 中把 `entry` 改为 `.dll` 路径

### AngelScript / Emma 插件不工作

1. 检查语法——AngelScript 要分号，Emma 用 `end` 结尾
2. 确认 `on_load` 函数名拼写正确
3. AngelScript 的 `@` 句柄引用（如 `@ctx = c;`）是赋值语法，不能省略
4. Emma 的 `fn` 关键字后必须有函数名和括号

### 面板注册了但不显示

1. 确认 render 回调是一个可调用的函数引用（AngelScript 用 `@func_name`，Emma 直接传函数名）
2. 确认 render 返回了有效的 UI spec（用 `ctx.ui.panel(...)` 或 `ctx.ui_panel(...)` 构建）
3. 在插件管理面板中检查插件状态和日志
