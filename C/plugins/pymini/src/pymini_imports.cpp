// pymini_imports.cpp — module resolution: sys.modules, stdlib factories,
// plugin-relative imports, package __init__ execution, vendor/libs search.
//
// Search order for `import x.y.z`:
//   1. interpreter.stdlib_factories (native stdlib-lite modules)
//   2. already-loaded sys.modules
//   3. per-frame module root → package-relative then absolute file search
//      across cfg.module_dirs in order (plugin root first, then engine
//      libs/vendor dirs)
// Package resolution:  <dir>/x/y/z.py  OR  <dir>/x/y/z/__init__.py
#include "pymini_interp.h"
#include "pymini_parser.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sao::plugins::pymini {
namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// run a file as a module; keeps module registered in sys.modules
PyRef exec_file(interpreter& i, const std::string& logical_name,
                const fs::path& p, const std::string& package) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        i.raise_exc("ImportError",
                    "cannot open module file '" + narrow(p.wstring()) + "'",
                    {});
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();

    PyRef mod = i.create_module_object(logical_name, narrow(p.wstring()),
                                       package);
    i.register_module(mod);
    i.pending.push_back(mod);
    try {
        feature_flags flags{};
        ast_module m = parse_source(src, narrow(p.wstring()), &flags);
        auto* modp = as_module(mod);
        frame f;
        f.locals = modp->dict;
        f.globals = modp->dict;
        f.fn_name = "<module>";
        f.file = narrow(p.wstring());
        i.collect_scope_decls(m.body, f);
        i.exec_body(m.body, f);
    } catch (...) {
        i.pending.pop_back();
        dict_del(as_dict(i.sys_modules), py_str(logical_name));
        throw;
    }
    i.pending.pop_back();
    return mod;
}

// locate a module file for `dotted` under a base dir; returns path + whether
// it's a package __init__.
bool find_module_path(const fs::path& base, const std::string& dotted,
                      fs::path* out_file, bool* is_pkg) {
    fs::path dir = base;
    std::size_t start = 0;
    while (true) {
        const auto dot = dotted.find('.', start);
        const std::string seg =
            dotted.substr(start, dot == std::string::npos
                                      ? std::string::npos
                                      : dot - start);
        const bool last = dot == std::string::npos;
        if (last) {
            fs::path file = dir / (seg + ".py");
            if (fs::exists(file)) {
                *out_file = file;
                *is_pkg = false;
                return true;
            }
            fs::path pkg = dir / seg / "__init__.py";
            if (fs::exists(pkg)) {
                *out_file = pkg;
                *is_pkg = true;
                return true;
            }
            return false;
        }
        dir = dir / seg;
        // intermediate levels must be packages (contain __init__.py) — allow
        // namespace packages too (dir exists without __init__.py)
        if (!fs::is_directory(dir))
            return false;
        start = dot + 1;
    }
}

// resolve `level` dots + `dotted` relative to the frame's __package__.
std::string resolve_relative(frame* f, const std::string& dotted, int level) {
    if (level == 0 || f == nullptr)
        return dotted;
    const auto* g = as_dict(f->globals);
    std::string pkg;
    if (g) {
        if (PyRef p = dict_get(g, py_str("__package__")))
            if (auto* ps = as_str(p))
                pkg = ps->v;
    }
    // strip `level - 1` trailing components
    for (int k = 1; k < level && !pkg.empty(); ++k) {
        const auto dot = pkg.rfind('.');
        pkg = dot == std::string::npos ? "" : pkg.substr(0, dot);
    }
    if (pkg.empty())
        return dotted;
    return dotted.empty() ? pkg : pkg + "." + dotted;
}

} // namespace

// ── import_dotted ─────────────────────────────────────────────────────────
PyRef interpreter::import_dotted(const std::string& dotted_in, frame* from,
                                 int level) {
    gil_guard g(*this);
    std::string dotted = resolve_relative(from, dotted_in, level);
    if (dotted.empty()) {
        // `from . import x` → the package itself
        if (from) {
            const auto* from_globals = as_dict(from->globals);
            if (from_globals) {
                if (PyRef p = dict_get(from_globals, py_str("__package__")))
                    if (auto* ps = as_str(p))
                        dotted = ps->v;
            }
        }
    }
    if (dotted.empty())
        raise_exc("ImportError", "cannot resolve empty import", {});

    // 1) sys.modules cache (covers stdlib + previously imported + builtins)
    if (PyRef m = find_loaded(dotted))
        return m;

    // 2) native stdlib factory (full dotted tail too: "collections.deque"?)
    auto it = stdlib_factories.find(dotted);
    if (it != stdlib_factories.end()) {
        PyRef mod = it->second(*this);
        register_module(mod);
        return mod;
    }
    // root segment for stdlib with submodule access (os.path)
    const auto dot = dotted.find('.');
    const std::string root = dotted.substr(0, dot);
    const auto rit = stdlib_factories.find(root);
    if (rit != stdlib_factories.end()) {
        PyRef root_mod = rit->second(*this);
        register_module(root_mod);
        if (dot == std::string::npos)
            return root_mod;
        PyRef v = getattr(root_mod, dotted.substr(dot + 1));
        if (v)
            return v;
        // fall through to file search
    }

    // 3) file search — intermediate packages are executed first
    const auto* from_g = from ? as_dict(from->globals) : nullptr;
    fs::path plugin_root(cfg.plugin_root);
    // package path of the importing module for level-anchored search
    std::string from_pkg;
    if (from_g) {
        if (PyRef p = dict_get(from_g, py_str("__package__")))
            if (auto* ps = as_str(p))
                from_pkg = ps->v;
    }

    // search each directory for the TOP-level name then descend
    PyRef leaf;
    const std::vector<fs::path> dirs = [&] {
        std::vector<fs::path> out;
        // importing module's own package dir first (for relative-ish lookup)
        if (!from_pkg.empty() && level == 0) {
            fs::path pkg_dir = plugin_root;
            // nothing additional — absolute imports still start at root
        }
        for (const auto& d : cfg.module_dirs)
            out.emplace_back(d);
        if (out.empty() && !cfg.plugin_root.empty())
            out.emplace_back(cfg.plugin_root);
        return out;
    }();

    for (const auto& base : dirs) {
        fs::path cur = base;
        std::size_t start = 0;
        leaf = nullptr;
        PyRef parent;
        bool failed = false;
        while (start <= dotted.size()) {
            const auto dpos = dotted.find('.', start);
            const std::string seg =
                dotted.substr(start, dpos == std::string::npos
                                          ? std::string::npos
                                          : dpos - start);
            const bool last = dpos == std::string::npos;
            fs::path file;
            bool is_pkg = false;
            fs::path dummy;
            // try seg.py then seg/__init__.py — direct probe per level
            fs::path cand_file = cur / (seg + ".py");
            fs::path cand_pkg = cur / seg / "__init__.py";
            if (fs::exists(cand_file)) {
                file = cand_file;
                is_pkg = false;
            } else if (fs::exists(cand_pkg)) {
                file = cand_pkg;
                is_pkg = true;
            } else {
                failed = true;
                break;
            }
            const std::string logical = dotted.substr(0, dpos);
            // parent package name for __package__
            const auto pdot = logical.rfind('.');
            const std::string parent_pkg =
                is_pkg ? logical
                       : (pdot == std::string::npos ? logical
                                                  : logical.substr(0, pdot));
            if (PyRef cached = find_loaded(logical)) {
                leaf = cached;
            } else {
                leaf = exec_file(*this, logical, file, parent_pkg);
            }
            // attach the child onto its parent package — `import a.b` must
            // make `a.b` reachable as an attribute of module `a`.
            if (parent) {
                if (auto* pm = as_module(parent))
                    dict_set(as_dict(pm->dict), py_str(seg), leaf);
            }
            parent = leaf;
            if (last)
                break;
            cur = cur / seg;
            start = dpos + 1;
        }
        if (failed || !leaf)
            continue;
        return leaf;
    }

    raise_exc("ModuleNotFoundError",
              "No module named '" + dotted + "'", {});
}

PyRef interpreter::import_from(PyRef module, const std::string& name) {
    PyRef v = getattr(module, name);
    if (v)
        return v;
    raise_exc("ImportError",
              "cannot import name '" + name + "' from module", {});
}

// ── cpython_only_roots ────────────────────────────────────────────────────
const std::unordered_set<std::string>& interpreter::cpython_only_roots() {
    static const std::unordered_set<std::string> roots = {
        // C-extension / eval-only roots that pymini deliberately cannot
        // emulate.  Kept in sync with docs/legacy-compat-design.md §subset.
        "ctypes", "cffi", "_ctypes", "numpy", "scipy", "pandas", "PIL",
        "cv2", "pygame", "wx", "PyQt5", "PyQt6", "PySide2", "PySide6",
        "tkinter", "multiprocessing", "subprocess", "signal", "socket",
        "ssl", "sqlite3", "_socket", "_ssl", "_sqlite3", "select",
        "selectors", "asyncio", "concurrent", "inspect", "dis", "marshal",
        "gc", "weakref", "builtins", "_imp", "importlib", "site",
        "sysconfig", "ensurepip", "pip", "setuptools", "distutils",
        "code", "codeop", "pdb", "bdb", "trace", "cProfile", "profile",
        "pickle", "shelve", "dbm", "ctypes", "mmap", "array_ffi",
        "requests", "urllib3", "aiohttp", "flask", "django",
        "unittest", "pytest", "doctest", "ctypes_test",
        "turtle", "pydoc", "idlelib", "lib2to3", "tabnanny",
        "pty", "tty", "termios", "fcntl", "pwd", "grp", "msvcrt",
        "winreg", "winsound", "msilib", "spwd", "resource", "syslog",
        "posix", "posixpath_c", "_winapi", "_curses", "curses",
        "_elementtree", "_decimal", "_bisect_impl", "_blake2", "_bz2",
        "_codecs", "_collections_impl", "_compat_pickle", "_contextvars",
        "_csv", "_datetime_impl", "_functools_impl", "_hashlib", "_heapq",
        "_io_c", "_json_impl", "_locale_impl", "_lsprof", "_lzma",
        "_markupbase", "_md5", "_multibytecodec", "_opcode", "_operator",
        "_pickle", "_posixsubprocess", "_py_abc", "_queue_impl",
        "_random_impl", "_sha1", "_sha2", "_sha3", "_socket_impl",
        "_sre_impl", "_stat", "_statistics_impl", "_struct_impl",
        "_symtable", "_thread_impl", "_tkinter", "_tracemalloc", "_typing",
        "_uuid_impl", "_warnings_impl", "_weakref_impl", "_zoneinfo",
        "xxlimited", "zipimport",
    };
    return roots;
}

// ── subset preflight (shared by bridge + loader adapter) ────────────
// Cheap lexical scan: true when the source stays inside the pymini subset —
// no cpython-only imports, no async/await, no generators.
bool pymini_preflight_subset(std::string_view src,
                             std::string* out_reason) {
    auto fail = [&](std::string why) {
        if (out_reason)
            *out_reason = std::move(why);
        return false;
    };
    // raw feature tokens — string-scrubbed light check
    const char* const bad_tokens[] = {"async def", "await ", "yield ",
                                      "yield\n",  "async with", "async for",
                                      nullptr};
    for (std::size_t t = 0; bad_tokens[t]; ++t)
        if (src.find(bad_tokens[t]) != std::string_view::npos)
            return fail(std::string("contains '") + bad_tokens[t] + "'");
    // import-root scan: pull first dotted segment of `import x` / `from x`
    const auto& roots = interpreter::cpython_only_roots();
    std::string_view cur = src;
    while (!cur.empty()) {
        const auto nl = cur.find('\n');
        std::string_view line =
            nl == std::string_view::npos ? cur : cur.substr(0, nl);
        cur = nl == std::string_view::npos ? "" : cur.substr(nl + 1);
        // strip leading ws + comment
        const auto s0 = line.find_first_not_of(" \t");
        if (s0 == std::string_view::npos || line[s0] == '#')
            continue;
        line = line.substr(s0);
        std::string_view verb;
        if (line.substr(0, 7) == "import ")
            verb = line.substr(7);
        else if (line.substr(0, 5) == "from ")
            verb = line.substr(5);
        else
            continue;
        // relative-from: `from .x import y` — skip leading dots
        while (!verb.empty() && verb.front() == '.')
            verb = verb.substr(1);
        const auto sp =
            verb.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.");
        std::string_view mod = sp == std::string_view::npos
                                   ? verb
                                   : verb.substr(0, sp);
        const auto dot = mod.find('.');
        const std::string root(
            dot == std::string_view::npos ? mod : mod.substr(0, dot));
        if (roots.count(root))
            return fail("imports cpython-only module '" + root + "'");
    }
    if (out_reason)
        out_reason->clear();
    return true;
}

} // namespace sao::plugins::pymini
