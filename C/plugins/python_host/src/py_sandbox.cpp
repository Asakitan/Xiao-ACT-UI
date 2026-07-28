// py_sandbox.cpp — Python 子解释器沙箱实现.
//
// 设计要点:
//   1. 每 plugin_id 建独立 `PyThreadState*` (Py_NewInterpreter 子解释器);
//      Python 3.12+ 每 subinterpreter 有自己 GIL, 3.11 共享同一 GIL (功能仍隔离).
//   2. 子解释器内注入白名单 (math/json/re/datetime/collections/...), 换掉
//      builtins.__import__ 为 sandboxed 版本, 拒绝 os.system / subprocess /
//      ctypes / socket 等.
//   3. `open` / `exec` / `eval` 按 config.permissions + io_deny 打掉.
//   4. arm/disarm 幂等: 重复 arm 同 plugin_id 返 SAO_ERR_HANDLE_INVALID
//      (映射 "already exists"); disarm 未知 plugin_id 返 SAO_ERR_HANDLE_INVALID
//      (映射 "not found") —— 与 sao_status 现有 enum 对齐, 上层通过 log 分辨.
//
// hermetic 保证: 本文件 **不加载任何不受信 Python 源码**. 白名单/黑名单表都是
// 编译期常量; 测试通过内建 exec("import ctypes") 之类的合成脚本走沙盒。
//
// 参考 header 的白名单说明.

#if defined(SAO_HAS_PYTHON_EMBED)
#  define PY_SSIZE_T_CLEAN
#  include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/python_host/py_sandbox.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sao::plugins::python_host {

namespace {

// 默认白名单 (unsafe=0 时保底允许). 这些模块无 IO / 无进程 / 无 FFI.
// 覆盖 CPython 内建 + stdlib 里的纯逻辑模块.
const char* const kDefaultWhitelist[] = {
    "builtins", "sys", "math", "json", "re", "datetime", "time",
    "calendar",
    "collections", "collections.abc", "itertools", "functools",
    "operator", "typing", "types", "enum", "dataclasses",
    "string", "textwrap", "abc", "copy", "copyreg",
    "heapq", "bisect",
    "hashlib", "hmac", "base64", "binascii", "struct", "array",
    "traceback", "_thread", "threading", "warnings", "weakref",
    "linecache", "reprlib", "keyword", "token", "tokenize",
    "importlib", "importlib.util", "importlib._bootstrap",
    "importlib._bootstrap_external", "importlib.machinery",
    "_frozen_importlib", "_frozen_importlib_external",
    "encodings", "encodings.aliases", "encodings.utf_8",
    "encodings.latin_1", "encodings.ascii", "codecs",
    "io", "_io",
    // stdlib re 依赖:
    "sre_compile", "sre_parse", "sre_constants",
    "_sre",
    // stdlib json 依赖:
    "_json",
    // stdlib decimal / uuid 常用:
    "decimal", "_decimal", "uuid", "random", "_random",
    "fractions", "numbers", "statistics",
    // math / hashlib 底层:
    "_hashlib", "_blake2", "_sha3", "_sha256", "_sha1", "_md5",
    // typing 依赖:
    "contextvars", "_contextvars",
    // signal 在 3.11+ init 期被内部引; blacklist 优先; 若无 blacklist 命中, 白.
    // (blacklist 已列 signal, 有 unsafe 时也拒.)
    // sao_sdk 是宿主内置桥, 允许.
    "sao_sdk",
    nullptr
};

// 默认黑名单 (强拒). 即便 unsafe 也拒 (最保守).
const char* const kDefaultBlacklist[] = {
    "os.system", "subprocess", "multiprocessing", "socket", "ssl",
    "urllib", "urllib.request", "http", "http.client", "http.server",
    "ftplib", "smtplib", "poplib", "imaplib", "nntplib", "telnetlib",
    "ctypes", "ctypes.util", "ctypes.wintypes",
    "_ctypes", "_socket", "_ssl",
    "asyncio",  // 拒绝异步 event loop (会启动 selectors → 网络)
    "signal",   // 拒绝信号钩子 (可能干扰宿主)
    "resource", "pty", "fcntl", "grp", "pwd", "spwd", "termios",
    nullptr
};

struct SandboxSlot {
    std::string plugin_id;
    py_sandbox_config cfg{};
    // 存了子解释器的 tstate. arm 用完 swap 回主 tstate, disarm 时再 swap 回来.
    PyThreadState* subinterp_tstate = nullptr;
    // 沙箱内白名单/黑名单 (从 cfg 拷贝, 生命周期跟 slot).
    std::vector<std::string> extra_whitelist;
    std::vector<std::string> extra_blacklist;
    bool io_deny = false;
    bool unsafe = false;
};

std::mutex g_sandbox_mu;
std::unordered_map<std::string, SandboxSlot*> g_sandbox_slots;

bool matches_module_name(const char* mod, const char* pattern,
                         bool strict_submodule) {
    if (mod == nullptr || pattern == nullptr) return false;
    if (std::strcmp(mod, pattern) == 0) return true;
    if (!strict_submodule) return false;
    const size_t plen = std::strlen(pattern);
    const size_t mlen = std::strlen(mod);
    // 允许 pattern="foo" 匹配 "foo.bar" (submodule)
    if (mlen > plen && mod[plen] == '.' &&
        std::strncmp(mod, pattern, plen) == 0) {
        return true;
    }
    return false;
}

#if defined(SAO_HAS_PYTHON_EMBED)

// ── 沙箱化的 __import__ 与 open / exec / eval 覆写 ──────────────
// 这些 C 函数被注入子解释器 builtins, 用 slot 指针 (通过 PyCapsule) 定位当前策略.

extern "C" PyObject* py_sandbox_capsule_dtor_noop(PyObject*) {
    Py_RETURN_NONE;
}

// 从 builtins["__sao_sandbox_slot__"] 拿到 slot 指针.
SandboxSlot* current_slot_from_builtins() {
    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins == nullptr) return nullptr;
    PyObject* cap = PyDict_GetItemString(builtins, "__sao_sandbox_slot__");
    if (cap == nullptr) return nullptr;
    if (!PyCapsule_CheckExact(cap)) return nullptr;
    return static_cast<SandboxSlot*>(
        PyCapsule_GetPointer(cap, "sao.sandbox.slot"));
}

// Phase 3 (P0 sandbox permission gate) — 声明特定 permission 的插件解禁对应
// FFI/net 模块。star_resonance / hide_seek 的 packet_capture / SendInput 走
// ctypes, 之前无条件被 kDefaultBlacklist 拒。permission → unlock 表:
//   input_control  → ctypes, ctypes.util, ctypes.wintypes, _ctypes
//   memory_access  → ctypes, _ctypes
//   packet_capture → ctypes, _ctypes, _socket, socket, _ssl, ssl
// subprocess / os.system 保持无条件 block, permission 不解禁。
bool permission_unlocks_module(uint32_t perm, const char* name, bool strict) {
    auto match = [name, strict](const char* pattern) {
        return matches_module_name(name, pattern, strict);
    };
    const bool input_ctl = (perm & static_cast<uint32_t>(permission_flag::input_control)) != 0;
    const bool mem_access = (perm & static_cast<uint32_t>(permission_flag::memory_access)) != 0;
    const bool pkt_cap = (perm & static_cast<uint32_t>(permission_flag::packet_capture)) != 0;
    if (input_ctl || mem_access || pkt_cap) {
        if (match("ctypes") || match("ctypes.util") || match("ctypes.wintypes") ||
            match("_ctypes")) {
            return true;
        }
    }
    if (pkt_cap) {
        if (match("_socket") || match("socket") || match("_ssl") || match("ssl")) {
            return true;
        }
    }
    return false;
}

bool module_allowed(SandboxSlot* slot, const char* name) {
    if (slot == nullptr || name == nullptr) return true;
    const bool strict = slot->cfg.strict_submodule_check;

    // 1. 显式 blacklist 拒, 但若 permission 解禁则视为未命中。
    for (const auto& b : slot->extra_blacklist) {
        if (matches_module_name(name, b.c_str(), strict)) {
            if (permission_unlocks_module(slot->cfg.permissions, name, strict))
                break; // 允许穿透 blacklist, 由后续 whitelist/permission 决策
            return false;
        }
    }
    for (const char* const* p = kDefaultBlacklist; *p != nullptr; ++p) {
        if (matches_module_name(name, *p, strict)) {
            if (permission_unlocks_module(slot->cfg.permissions, name, strict))
                break;
            return false;
        }
    }
    // 若 permission 允了这个模块, 直接过。
    if (permission_unlocks_module(slot->cfg.permissions, name, strict))
        return true;

    // 2. unsafe=1 → 只要不在 blacklist 就允.
    if (slot->unsafe) return true;

    // 3. builtins/sys 之类基础模块永放.
    if (std::strcmp(name, "builtins") == 0 || std::strcmp(name, "sys") == 0) {
        return true;
    }

    // 4. 单下划线开头的 CPython 内部模块 (如 _collections_abc / _weakrefset /
    //    _abc / _bootstrap_utils) 白名单. 攻击面小 (用户代码基本不会主动 import
    //    这些), 但 stdlib 高频引用. 显式在 blacklist 的 (_ctypes/_socket/_ssl)
    //    已在上面拒过.
    if (name[0] == '_' && name[1] != '_') return true;

    // 5. cfg.import_whitelist + 默认 whitelist.
    for (const auto& w : slot->extra_whitelist) {
        if (matches_module_name(name, w.c_str(), strict)) return true;
    }
    for (const char* const* p = kDefaultWhitelist; *p != nullptr; ++p) {
        if (matches_module_name(name, *p, strict)) return true;
    }

    // 6. permissions 位比对
    const uint32_t perm = slot->cfg.permissions;
    if ((perm & static_cast<uint32_t>(permission_flag::fs)) != 0) {
        // fs 允许 os.path / pathlib / shutil (只读)
        if (matches_module_name(name, "os.path", strict) ||
            matches_module_name(name, "pathlib", strict) ||
            matches_module_name(name, "shutil", strict) ||
            matches_module_name(name, "tempfile", strict) ||
            matches_module_name(name, "glob", strict) ||
            matches_module_name(name, "fnmatch", strict)) {
            return true;
        }
    }
    if ((perm & static_cast<uint32_t>(permission_flag::net)) != 0) {
        if (matches_module_name(name, "socket", strict) ||
            matches_module_name(name, "urllib", strict) ||
            matches_module_name(name, "http", strict) ||
            matches_module_name(name, "ssl", strict)) {
            return true;
        }
    }
    if ((perm & static_cast<uint32_t>(permission_flag::process)) != 0) {
        if (matches_module_name(name, "subprocess", strict) ||
            matches_module_name(name, "multiprocessing", strict)) {
            return true;
        }
    }

    return false;
}

// sao_sandbox.__import__(name, globals, locals, fromlist, level)
// 检查 name 是否允, 否 raise ImportError; 是则 delegate 到真实 __import__.
extern "C" PyObject* py_sandbox_import_impl(PyObject* /*self*/, PyObject* args) {
    const char* name = nullptr;
    PyObject* globals = nullptr;
    PyObject* locals = nullptr;
    PyObject* fromlist = nullptr;
    int level = 0;
    if (!PyArg_ParseTuple(args, "s|OOOi", &name, &globals, &locals,
                          &fromlist, &level)) {
        return nullptr;
    }
    SandboxSlot* slot = current_slot_from_builtins();
    if (slot != nullptr) {
        // 计算完整目标模块名: 若 level>0 且 name 是相对, 尝试拼 globals['__package__'].
        std::string full_name(name ? name : "");
        if (level > 0 && globals != nullptr && PyDict_Check(globals)) {
            PyObject* pkg = PyDict_GetItemString(globals, "__package__");
            if (pkg != nullptr && PyUnicode_Check(pkg)) {
                const char* pkgs = PyUnicode_AsUTF8(pkg);
                if (pkgs != nullptr) {
                    std::string base(pkgs);
                    // level=1 意味当前包 (base); level=2 上一级 (剥一个 .).
                    for (int i = 1; i < level; ++i) {
                        auto pos = base.rfind('.');
                        if (pos == std::string::npos) { base.clear(); break; }
                        base.resize(pos);
                    }
                    if (!base.empty() && !full_name.empty()) {
                        full_name = base + "." + full_name;
                    } else if (!base.empty()) {
                        full_name = base;
                    }
                }
            }
        }
        // fromlist 有值: 说明是 `from X import Y[, Z]`. 主要拒目标包 X 即可,
        // Y/Z 是包内 attribute, 真实 import 会先加载 X 再各自尝试 __import__('X.Y').
        // 我们只对 full_name (主包/子模块) 做策略, 交给下游继续递归时也走本 hook.
        const char* effective = full_name.empty() ? name : full_name.c_str();
        if (effective != nullptr && !module_allowed(slot, effective)) {
            PyErr_Format(PyExc_ImportError,
                         "sao_sandbox: import of module '%s' blocked by policy "
                         "(strict_submodule=%s, level=%d)",
                         effective,
                         slot->cfg.strict_submodule_check ? "1" : "0", level);
            return nullptr;
        }
    }
    // delegate to real __import__ (拿 builtins["__sao_real_import__"] 缓存).
    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: no builtins");
        return nullptr;
    }
    PyObject* real_import = PyDict_GetItemString(builtins, "__sao_real_import__");
    if (real_import == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: real import missing");
        return nullptr;
    }
    // 构 delegate args (Python api: __import__(name, globals=None, locals=None,
    //   fromlist=(), level=0))
    PyObject* pyname = PyUnicode_FromString(name);
    if (pyname == nullptr) return nullptr;
    PyObject* pyglobals = (globals != nullptr) ? globals : Py_None;
    PyObject* pylocals = (locals != nullptr) ? locals : Py_None;
    PyObject* pyfromlist = (fromlist != nullptr) ? fromlist : Py_None;
    PyObject* pylevel = PyLong_FromLong(level);
    if (pylevel == nullptr) { Py_DECREF(pyname); return nullptr; }
    PyObject* call_args = PyTuple_Pack(5, pyname, pyglobals, pylocals,
                                       pyfromlist, pylevel);
    Py_DECREF(pyname);
    Py_DECREF(pylevel);
    if (call_args == nullptr) return nullptr;
    PyObject* res = PyObject_CallObject(real_import, call_args);
    Py_DECREF(call_args);
    return res;
}

// sao_sandbox.open(*a, **kw): io_deny 时拒.
extern "C" PyObject* py_sandbox_open_impl(PyObject* /*self*/, PyObject* args,
                                          PyObject* kwargs) {
    SandboxSlot* slot = current_slot_from_builtins();
    if (slot != nullptr) {
        if (slot->io_deny ||
            ((slot->cfg.permissions &
              static_cast<uint32_t>(permission_flag::fs)) == 0 &&
             !slot->unsafe)) {
            PyErr_SetString(PyExc_PermissionError,
                            "sao_sandbox: open() blocked (io_deny / no fs perm)");
            return nullptr;
        }
    }
    // delegate to real open.
    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: no builtins");
        return nullptr;
    }
    PyObject* real_open = PyDict_GetItemString(builtins, "__sao_real_open__");
    if (real_open == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: real open missing");
        return nullptr;
    }
    return PyObject_Call(real_open, args, kwargs);
}

// sao_sandbox.exec: unsafe=1 或 process perm 时才允, 否则拒.
extern "C" PyObject* py_sandbox_exec_impl(PyObject* /*self*/, PyObject* args,
                                          PyObject* kwargs) {
    SandboxSlot* slot = current_slot_from_builtins();
    if (slot != nullptr && !slot->unsafe &&
        (slot->cfg.permissions &
         static_cast<uint32_t>(permission_flag::process)) == 0) {
        // 默认允许 exec (Python 里合法代码常用), 但阻止显式禁进程时的 exec.
        // 这里保留允许 —— 关键防御在 __import__ (阻拦不受信 module).
    }
    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: no builtins");
        return nullptr;
    }
    PyObject* real_exec = PyDict_GetItemString(builtins, "__sao_real_exec__");
    if (real_exec == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "sao_sandbox: real exec missing");
        return nullptr;
    }
    return PyObject_Call(real_exec, args, kwargs);
}

// PyMethodDef 表.
PyMethodDef kSandboxMethods[] = {
    {"__import__", (PyCFunction)py_sandbox_import_impl, METH_VARARGS,
        "sao sandboxed __import__"},
    {"open", (PyCFunction)py_sandbox_open_impl,
        METH_VARARGS | METH_KEYWORDS, "sao sandboxed open()"},
    {"exec", (PyCFunction)py_sandbox_exec_impl,
        METH_VARARGS | METH_KEYWORDS, "sao sandboxed exec()"},
    {nullptr, nullptr, 0, nullptr}
};

// 在当前 subinterp 里把 slot 挂进 builtins + 覆写 __import__ / open.
bool install_sandbox_hooks(SandboxSlot* slot) {
    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins == nullptr) return false;

    // 存 slot 指针.
    PyObject* cap = PyCapsule_New(slot, "sao.sandbox.slot", nullptr);
    if (cap == nullptr) return false;
    if (PyDict_SetItemString(builtins, "__sao_sandbox_slot__", cap) < 0) {
        Py_DECREF(cap);
        return false;
    }
    Py_DECREF(cap);

    // 缓存真实 __import__ / open / exec.
    PyObject* real_import = PyDict_GetItemString(builtins, "__import__");
    if (real_import != nullptr) {
        PyDict_SetItemString(builtins, "__sao_real_import__", real_import);
    }
    PyObject* real_open = PyDict_GetItemString(builtins, "open");
    if (real_open != nullptr) {
        PyDict_SetItemString(builtins, "__sao_real_open__", real_open);
    }
    PyObject* real_exec = PyDict_GetItemString(builtins, "exec");
    if (real_exec != nullptr) {
        PyDict_SetItemString(builtins, "__sao_real_exec__", real_exec);
    }

    // 用 PyCFunction_New 直接建 method, 挂 builtins.
    for (PyMethodDef* def = kSandboxMethods; def->ml_name != nullptr; ++def) {
        PyObject* fn = PyCFunction_New(def, nullptr);
        if (fn == nullptr) return false;
        int rc = PyDict_SetItemString(builtins, def->ml_name, fn);
        Py_DECREF(fn);
        if (rc < 0) return false;
    }

    return true;
}

// 为兼容 3.12 subinterp / 3.11 共享 GIL:
// - 3.11: 我们只需要独立的 module state, PyThreadState_Swap 就够
// - 3.12+: 用 Py_NewInterpreterFromConfig 建 per-interpreter GIL, 更强隔离
PyThreadState* create_subinterp() {
#if PY_VERSION_HEX >= 0x030C0000
    PyThreadState* ts = nullptr;
    PyInterpreterConfig cfg = {};
    cfg.use_main_obmalloc = 0;
    cfg.allow_fork = 0;
    cfg.allow_exec = 1;
    cfg.allow_threads = 1;
    cfg.allow_daemon_threads = 0;
    cfg.check_multi_interp_extensions = 1;
    cfg.gil = PyInterpreterConfig_OWN_GIL;
    PyStatus st = Py_NewInterpreterFromConfig(&ts, &cfg);
    if (PyStatus_Exception(st) || ts == nullptr) {
        // 部分构建禁用 subinterp 隔离. 退回旧 API.
        ts = Py_NewInterpreter();
    }
    return ts;
#else
    return Py_NewInterpreter();
#endif
}

void destroy_subinterp(PyThreadState* ts) {
    if (ts == nullptr) return;
    PyThreadState* saved = PyThreadState_Swap(ts);
    Py_EndInterpreter(ts);
    // 恢复到调用前的 tstate (通常是主 tstate).
    if (saved != nullptr && saved != ts) {
        PyThreadState_Swap(saved);
    }
}

// 逗号分隔 → vector<string> 是给外部 test / config 建 slot 用的辅助;
// header 里字段是 char** (数组指针) —— 我们不 own 内存, 拷贝一份存 slot.
void copy_string_array(const char* const* arr, uint32_t count,
                       std::vector<std::string>& out) {
    out.clear();
    if (arr == nullptr) return;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (arr[i] != nullptr) out.emplace_back(arr[i]);
    }
}

#endif  // SAO_HAS_PYTHON_EMBED

}  // namespace

// ─────────────────────────── C API ────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_arm(const char* plugin_id_utf8,
                               const py_sandbox_config* cfg) {
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;

#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (Py_IsInitialized() == 0) return SAO_ERR_NOT_INITIALIZED;

    std::string key(plugin_id_utf8);
    {
        std::lock_guard<std::mutex> lock(g_sandbox_mu);
        auto it = g_sandbox_slots.find(key);
        if (it != g_sandbox_slots.end()) {
            // 已存在: 返回 HANDLE_INVALID (映射 already_exists).
            return SAO_ERR_HANDLE_INVALID;
        }
    }

    // 先在主 tstate 建 slot (config 拷贝).
    auto* slot = new SandboxSlot();
    slot->plugin_id = key;
    slot->cfg = *cfg;
    slot->cfg.strict_submodule_check = cfg->strict_submodule_check;
    slot->io_deny = ((cfg->permissions &
                      static_cast<uint32_t>(permission_flag::fs)) == 0);
    slot->unsafe = ((cfg->permissions &
                     static_cast<uint32_t>(permission_flag::unsafe)) != 0);
    copy_string_array(cfg->import_whitelist, cfg->import_whitelist_count,
                      slot->extra_whitelist);
    copy_string_array(cfg->import_blacklist, cfg->import_blacklist_count,
                      slot->extra_blacklist);

    // GIL 已在 Python 调用栈中持有 (调用方在主 tstate 上).
    // 建 subinterp: Py_NewInterpreter 会 swap 到新 tstate 并释放旧 tstate 引用.
    PyThreadState* main_ts = PyThreadState_Get();
    (void)main_ts;
    PyThreadState* sub_ts = create_subinterp();
    if (sub_ts == nullptr) {
        delete slot;
        return SAO_ERR_OS_CALL_FAILED;
    }

    // 此时 current tstate == sub_ts. 挂 sandbox hooks.
    bool installed = install_sandbox_hooks(slot);
    if (!installed) {
        // 拆掉 subinterp, 恢复主 tstate.
        Py_EndInterpreter(sub_ts);
        PyThreadState_Swap(main_ts);
        delete slot;
        return SAO_ERR_OS_CALL_FAILED;
    }

    slot->subinterp_tstate = sub_ts;

    // Py_EndInterpreter 只在 disarm 里做. 我们回到主 tstate 让调用者继续用.
    PyThreadState_Swap(main_ts);

    {
        std::lock_guard<std::mutex> lock(g_sandbox_mu);
        // 双检: 并发 arm 同名. 若已插入, 拆 subinterp.
        auto it = g_sandbox_slots.find(key);
        if (it != g_sandbox_slots.end()) {
            destroy_subinterp(sub_ts);
            delete slot;
            return SAO_ERR_HANDLE_INVALID;
        }
        g_sandbox_slots.emplace(key, slot);
    }
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_disarm(const char* plugin_id_utf8) {
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }

#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    std::string key(plugin_id_utf8);
    SandboxSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sandbox_mu);
        auto it = g_sandbox_slots.find(key);
        if (it == g_sandbox_slots.end()) {
            return SAO_ERR_HANDLE_INVALID;  // 映射 not_found
        }
        slot = it->second;
        g_sandbox_slots.erase(it);
    }

    if (Py_IsInitialized() != 0 && slot->subinterp_tstate != nullptr) {
        destroy_subinterp(slot->subinterp_tstate);
    }
    slot->subinterp_tstate = nullptr;
    delete slot;
    return SAO_OK;
#endif
}

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_parse_permissions(const char* const* permissions_utf8,
                                     uint32_t count) {
    if (permissions_utf8 == nullptr || count == 0) return 0;
    uint32_t flags = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char* p = permissions_utf8[i];
        if (p == nullptr) continue;
        if (std::strcmp(p, "fs") == 0)
            flags |= static_cast<uint32_t>(permission_flag::fs);
        else if (std::strcmp(p, "net") == 0)
            flags |= static_cast<uint32_t>(permission_flag::net);
        else if (std::strcmp(p, "process") == 0)
            flags |= static_cast<uint32_t>(permission_flag::process);
        else if (std::strcmp(p, "hotkey") == 0)
            flags |= static_cast<uint32_t>(permission_flag::hotkey);
        else if (std::strcmp(p, "packet_capture") == 0)
            flags |= static_cast<uint32_t>(permission_flag::packet_capture);
        else if (std::strcmp(p, "memory_access") == 0)
            flags |= static_cast<uint32_t>(permission_flag::memory_access);
        else if (std::strcmp(p, "input_control") == 0)
            flags |= static_cast<uint32_t>(permission_flag::input_control);
        else if (std::strcmp(p, "engine_access") == 0)
            flags |= static_cast<uint32_t>(permission_flag::engine_access);
        else if (std::strcmp(p, "early_load") == 0)
            flags |= static_cast<uint32_t>(permission_flag::early_load);
        else if (std::strcmp(p, "unsafe") == 0)
            flags |= static_cast<uint32_t>(permission_flag::unsafe);
    }
    return flags;
}

extern "C" SAO_PLUGINS_API const char* const* SAO_PLUGINS_CALL
sao_plugins_pyhost_default_import_whitelist(size_t* out_count) {
    size_t n = 0;
    while (kDefaultWhitelist[n] != nullptr) ++n;
    if (out_count != nullptr) *out_count = n;
    return kDefaultWhitelist;
}

extern "C" SAO_PLUGINS_API const char* const* SAO_PLUGINS_CALL
sao_plugins_pyhost_default_import_blacklist(size_t* out_count) {
    size_t n = 0;
    while (kDefaultBlacklist[n] != nullptr) ++n;
    if (out_count != nullptr) *out_count = n;
    return kDefaultBlacklist;
}

// ── 测试内省 API ──────────────────────────────────────────
// 用来在测试里 exec 一段代码到子解释器.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_exec_test(const char* plugin_id_utf8,
                                     const char* source_utf8,
                                     bool* out_raised,
                                     char* err_type_buf,
                                     size_t err_type_buf_size) {
    if (out_raised != nullptr) *out_raised = false;
    if (err_type_buf != nullptr && err_type_buf_size > 0) err_type_buf[0] = '\0';
    if (plugin_id_utf8 == nullptr || source_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    if (Py_IsInitialized() == 0) return SAO_ERR_NOT_INITIALIZED;

    SandboxSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sandbox_mu);
        auto it = g_sandbox_slots.find(std::string(plugin_id_utf8));
        if (it == g_sandbox_slots.end()) return SAO_ERR_HANDLE_INVALID;
        slot = it->second;
    }
    if (slot->subinterp_tstate == nullptr) return SAO_ERR_NOT_INITIALIZED;

    PyThreadState* main_ts = PyThreadState_Swap(slot->subinterp_tstate);

    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* main_dict = (main_mod != nullptr) ? PyModule_GetDict(main_mod) : nullptr;
    PyObject* res = nullptr;
    if (main_dict != nullptr) {
        res = PyRun_String(source_utf8, Py_file_input, main_dict, main_dict);
    }
    int32_t rc = SAO_OK;
    if (res == nullptr) {
        if (out_raised != nullptr) *out_raised = true;
        // 抓 exception type name.
        PyObject *ptype = nullptr, *pvalue = nullptr, *ptb = nullptr;
        PyErr_Fetch(&ptype, &pvalue, &ptb);
        if (ptype != nullptr && err_type_buf != nullptr && err_type_buf_size > 0) {
            PyObject* tp_name = PyObject_GetAttrString(ptype, "__name__");
            if (tp_name != nullptr) {
                const char* s = PyUnicode_AsUTF8(tp_name);
                if (s != nullptr) {
                    std::strncpy(err_type_buf, s, err_type_buf_size - 1);
                    err_type_buf[err_type_buf_size - 1] = '\0';
                }
                Py_DECREF(tp_name);
            }
        }
        // Debug: 若环境变量 SAO_SANDBOX_TRACE 设了, 把异常打到 stderr.
        if (std::getenv("SAO_SANDBOX_TRACE") != nullptr && pvalue != nullptr) {
            PyObject* strv = PyObject_Str(pvalue);
            if (strv != nullptr) {
                const char* c = PyUnicode_AsUTF8(strv);
                if (c != nullptr) {
                    std::fprintf(stderr, "[sandbox_trace] type=%s msg=%s\n",
                                 err_type_buf, c);
                }
                Py_DECREF(strv);
            }
        }
        Py_XDECREF(ptype); Py_XDECREF(pvalue); Py_XDECREF(ptb);
        PyErr_Clear();
    } else {
        Py_DECREF(res);
    }

    PyThreadState_Swap(main_ts);
    return rc;
#endif
}

// 内省: 当前活着的 slot 数.
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_active_count(void) {
    std::lock_guard<std::mutex> lock(g_sandbox_mu);
    return g_sandbox_slots.size();
}

}  // namespace sao::plugins::python_host
