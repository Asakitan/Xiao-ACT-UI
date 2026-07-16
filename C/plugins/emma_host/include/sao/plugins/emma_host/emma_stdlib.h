// emma_stdlib.h — Emma 侧标准库注册
//
// 对齐 Python 源: emma_runtime.py 的 _EmmaInterpreter._builtins + load_script /
// import / json_encode / json_decode / time / sleep 内置。
//
// 完整内置清单 (对齐 python _builtins 每一项):
//
// 基础转换:
//   - print(*args)          — 输出到 host log (对齐 python print → ctx.log)
//   - str(v)                — 转字符串, None → "nil"
//   - int(v)                — 转整数, None → 0
//   - float(v)              — 转浮点, None → 0.0
//   - tostring(v)           — 别名 str
//   - tonumber(v)           — 别名 float
//
// 容器:
//   - len(v)                — 数组/字典/字符串长度
//   - type(v)               — 返回类型名字符串
//   - pairs(d)              — 字典 (key, value) 元组数组
//   - ipairs(a)             — 数组 (index, value) 元组数组
//   - range(start, stop, step)  — 整数序列
//
// 数学:
//   - abs(v), min(...), max(...)
//   - floor, ceil, round, sqrt, sin, cos, tan
//
// IO / 时间:
//   - json_encode(v)        — v → utf-8 json 字符串
//   - json_decode(s)        — utf-8 json 字符串 → EmmaValue
//   - time()                — 当前 unix time (double)
//   - sleep(s)              — 阻塞 s 秒 (慎用, 不该在 UI 线程)
//
// 脚本加载 (由 IO stdlib 注入, 需要 plugin_base_dir):
//   - load_script(path)     — 加载 sibling .emma 文件到本 interp
//   - import(path)          — load_script 别名
//
// SDK (由 sdk_binding/binding_emma 注入, 需要 ctx):
//   - ctx                    — PluginContext 对象 (SDK 方法族入口)
//   - log(*args)            — ctx.log 简化别名
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

class interpreter;

// 装标准内置到 interp 的全局作用域: print/str/int/float/len/type/tostring/
// tonumber/pairs/ipairs/range/abs/min/max/floor/ceil/round/sqrt/sin/cos/tan。
// 对齐 python _builtins + 加数学扩展。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_stdlib(interpreter* interp);

// 装 IO 相关内置: json_encode/json_decode/time/sleep + load_script/import。
// plugin_base_dir 用于 load_script 的沙箱约束 (拒 .. 逃逸)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_io_stdlib(interpreter* interp,
                                   const wchar_t* plugin_base_dir);

// 装数学扩展 (可选, 默认由 install_stdlib 自动装)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_math_stdlib(interpreter* interp);

// 装 host log 桥 (让 print / log 内置直接调 host 回调, 不进 stdout)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_log_bridge(interpreter* interp,
                                    void (*log_fn)(const char* utf8, void* ud),
                                    void* user_data);

// 列出所有已安装的内置名 (供调试 / 文档生成)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_list_builtins(interpreter* interp,
                               const char*** out_names,
                               size_t* out_count);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_emma_free_names(const char** names, size_t count);

} // namespace sao::plugins::emma_host
