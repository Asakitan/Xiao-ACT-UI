# 插件打包与一键导入 Plugin packaging & one-click import

面向**插件作者**：怎样把一个插件做成可以让用户「一键导入、导入即用、无需编译」的包。

## TL;DR — 需要编译吗？

**纯 Python 插件不需要任何编译。** 加载器在运行时用 `importlib` 直接加载原始 `.py`，
dev 态与 onedir 冻结态（打包后的 exe）都一样——冻结 exe 内嵌了完整的 CPython 解释器。
用户端零编译、零命令行：在「插件管理 → 导入」选一个 `.zip` 即装即用。

只有两种情况需要作者在**打包前**做准备（用户端永远不编译）：

| 情况 | 作者怎么做 |
|---|---|
| 用到主程序没有的第三方库 | 写进 `requirements.txt`，并把**纯 Python 副本** vendor 进 `vendor/`（冻结态没有 pip） |
| 用到原生扩展 `.pyd`（Cython/C） | 预编译匹配目标 `cp3xx` + `win_amd64` 的 `.pyd` 随包；建议同时留纯 Python 回退 |

## 插件包结构（一个 .zip = 一个插件）

```
my_plugin.zip
├── plugin.json        # 清单（必需）
├── plugin.py          # 入口（必需）
├── requirements.txt   # 可选：第三方依赖声明
├── vendor/            # 可选：纯 Python 依赖副本（冻结/离线兜底，自动进 sys.path）
├── libs/              # 可选：dev 期 pip --target 缓存（一般不随包发）
└── assets/ ...        # 可选：资源
```

清单可以在 zip 根部，也可以在唯一的一层顶级文件夹里（`my_plugin/plugin.json`），两种都接受。

### plugin.json 最小示例

```json
{
  "id": "my_plugin",
  "name": "我的插件",
  "version": "1.0.0",
  "entry": "plugin.py",
  "enabled": true,
  "permissions": [],
  "capabilities": ["ui_panels"]
}
```

### plugin.py 钩子

```python
def on_load(ctx):
    # 若有第三方依赖：从插件自带的 vendor/ 加载（不污染全局 site-packages）
    ctx.ensure_requirements()          # 读 requirements.txt，按 libs→vendor→site 顺序满足
    ctx.register_ui_panel("my_panel", {"title": "我的面板"}, render=_render)

def on_enable():  ...
def on_disable(): ...
def on_unload():  ...   # 释放资源；定时器/订阅由平台自动清理
```

> 提示：加载器在执行 `plugin.py` **之前**就已把插件自带的 `vendor/`、`libs/` 前插
> `sys.path`，所以一个纯 Python 依赖只要丢进 `vendor/`，直接 `import` 即可，连
> `ctx.ensure_requirements()` 都可以不调。

## 打包成 .zip

用随仓库的作者工具：

```bash
python tools/pack_plugin.py plugins/my_plugin
# → my_plugin-1.0.0.zip（默认排除 __pycache__/.git/libs 等）
```

## 用户怎么导入

1. 打开「插件管理面板」（Entity 或 WebView 都行）。
2. 点工具条「导入 Import」→ 选 `.zip`。
3. 自动解压到更新不被覆盖的 `user_plugins/<id>/`、满足依赖、启用加载——**立即可用**。
4. 用户安装的插件卡片上会多出「卸载 Uninstall」（内置插件只能禁用，不可删）。

## 实现要点（给维护者）

- 安装：`act_platform/plugin_install.py`（zip-slip 防穿越解压 → `user_plugins/<id>/`）。
- 依赖引导：`act_platform/plugin_deps.py`（`libs`→`vendor`→`site` 顺序，冻结态跳过 pip）。
- 运行时动作：`act_plugin_import` / `act_plugin_import_dialog` / `act_plugin_uninstall`。
- 单插件热注册：`PluginManager.refresh_plugin()` 不会重启其他有状态插件。
- 回归：`python -m unittest tools.act_plugin_install_selftest`。
