# example_emma_plugin — Emma 插件示例骨架

## 需要的宿主

- ``emma_host`` (零依赖内置解释器; ``SAO_PLUGINS_ENABLE_EMMA=ON``)

## Emma 语法速查

| 语法 | 语义 |
|------|------|
| ``-- comment`` | 行注释 |
| ``let name = expr`` | 变量声明 |
| ``name = expr`` | 赋值 (若变量不存在会隐式声明) |
| ``fn name(args) ... end`` | 函数定义 |
| ``if cond ... elif ... else ... end`` | 条件 |
| ``while cond ... end`` | 循环 |
| ``for x in list ... end`` | 迭代 |
| ``return expr`` | 返回 |
| ``[a, b, c]`` | 数组字面量 |
| ``{k: v, k2: v2}`` | 字典字面量 |
| ``"str" .. "cat"`` | 字符串拼接 |
| ``and / or / not`` | 逻辑 |
| ``==, !=, <, <=, >, >=`` | 比较 |
| ``+, -, *, /, %`` | 算术 |
| ``obj.attr / obj.method(args)`` | 属性 + 方法调用 |
| ``obj[key]`` | 下标 |
| ``true / false / nil`` | 常量 |
| ``break / continue`` | 循环控制 |

## 内置函数

``print, str, int, float, len, type, tostring, tonumber, pairs, ipairs,
range, abs, min, max, json_encode, json_decode, time, sleep, load_script,
import``

## 从 Python 平台迁移

本例使用内置 Emma 宿主的 `on_load(ctx)` 与无参数 enable/disable/unload hooks；
停用和卸载时清理本例计时器。旧脚本仍须核对当前解释器语法及 ctx 方法，
已提供同名接口不代表全部旧平台行为均兼容。
