// pymini_stdlib2.cpp — stdlib-lite module factories (part 2):
//   os os.path sys io threading queue logging pathlib copy traceback
//   warnings abc typing contextlib bisect heapq itertools functools
//   collections dataclasses datetime enum
//
// datetime/enum/collections/dataclasses/pathlib are bootstrapped as pymini
// Python source via exec_module_source(); the rest are native factories.
#include "pymini_interp.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <thread>

namespace sao::plugins::pymini {

// ═══ pimpl bodies (must live at class namespace scope) ═══
struct PyFileObj::impl {
    FILE* fp = nullptr;
    bool binary = false;
    bool written = false;
    ~impl() {
        if (fp)
            std::fclose(fp);
    }
};
PyFileObj::PyFileObj() : PyObj(py_kind::file_) {}
PyFileObj::~PyFileObj() = default;

struct PyThreadObj::impl {
    PyRef target;
    py_args args;
    std::thread th;
    // shared flag — the detached worker may outlive this impl (and the
    // owning PyRef); a raw &done pointer would dangle.
    std::shared_ptr<std::atomic<bool>> done =
        std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> started{false};
    std::string name;
    ~impl() {
        // never join here: the worker itself needs the gil to run, and the
        // destructor typically executes under it → self-deadlock. Detach and
        // let the shared done flag carry the liveness signal.
        if (th.joinable())
            th.detach();
    }
};
PyThreadObj::PyThreadObj() : PyObj(py_kind::thread_) {}
PyThreadObj::~PyThreadObj() = default;

namespace {

PyRef mk_mod(const std::string& name) {
    auto m = std::make_shared<PyModuleObj>();
    m->name = name;
    m->dict = py_dict();
    m->package = name;
    return m;
}
void put_fn(PyRef m, const char* name, py_native_fn fn) {
    dict_set(as_dict(as_module(m)->dict), py_str(name),
             py_builtin(name, std::move(fn)));
}
void put_c(PyRef m, const char* name, PyRef v) {
    dict_set(as_dict(as_module(m)->dict), py_str(name), std::move(v));
}
// 非 noreturn 包装：调用点 raise 之后的 return 保持可达（消 C4702）。
void raise_system_exit(interpreter& i) {
    i.raise_exc("SystemExit", "sys.exit", {});
}
PyRef arg_at(interpreter& i, const py_args& a, std::size_t n,
             const char* fn) {
    if (n < a.pos.size())
        return a.pos[n];
    i.raise_exc("TypeError",
                std::string(fn) + " missing positional argument", {});
}

// local member_fn (receiver bound into arg0)
PyRef member_fn(const std::string& name, const PyRef& recv,
                py_native_fn fn) {
    return py_builtin(name,
                      [recv, fn = std::move(fn)](interpreter& i,
                                                 const py_args& a) {
                          py_args a2;
                          a2.pos.push_back(recv);
                          a2.pos.insert(a2.pos.end(), a.pos.begin(),
                                        a.pos.end());
                          a2.kw = a.kw;
                          return fn(i, a2);
                      });
}

// ═══ filesystem sandbox + helpers ═══
namespace sb {
inline bool cfg_root_empty(interpreter& i) {
    return i.cfg.plugin_root.empty();
}
std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}
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

// map a plugin path into the sandboxed root; rejects escapes.
std::wstring resolve_in_root(interpreter& i, const std::string& path) {
    if (cfg_root_empty(i)) {
        // no sandbox configured — treat as relative to process cwd
        return widen(path);
    }
    std::wstring rel = widen(path);
    for (auto& c : rel)
        if (c == L'/')
            c = L'\\';
    // strip drive/UNC prefixes — treat all plugin paths as root-relative
    if (rel.size() >= 2 && rel[1] == L':')
        rel = rel.substr(2);
    while (!rel.empty() && rel.front() == L'\\')
        rel.erase(rel.begin());
    if (rel.find(L"..") != std::wstring::npos)
        i.raise_exc("PermissionError",
                    "path escapes plugin sandbox: '" + path + "'", {});
    std::wstring full = i.cfg.plugin_root;
    if (!full.empty() && full.back() != L'\\')
        full += L'\\';
    return full + rel;
}

bool exists_w(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}
bool isdir_w(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int64_t mtime_w(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return 0;
    ULARGE_INTEGER u;
    u.LowPart = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL - 11644473600ULL);
}
int64_t size_w(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return -1;
    ULARGE_INTEGER u;
    u.LowPart = d.nFileSizeLow;
    u.HighPart = d.nFileSizeHigh;
    return static_cast<int64_t>(u.QuadPart);
}
void mkdirs_w(const std::wstring& p) {
    std::wstring cur;
    cur.reserve(p.size());
    std::size_t k = 0;
    while (k < p.size()) {
        const auto sep = p.find_first_of(L"\\/", k);
        cur = sep == std::wstring::npos ? p : p.substr(0, sep);
        if (!cur.empty() && cur.back() != L':' && !isdir_w(cur))
            CreateDirectoryW(cur.c_str(), nullptr);
        if (sep == std::wstring::npos)
            break;
        k = sep + 1;
    }
}
} // namespace sb

namespace file_impl {
PyFileObj* need_file(interpreter& i, const py_args& a, const char* fn) {
    auto* f = as_file(arg_at(i, a, 0, fn));
    if (!f || !f->pimpl || !f->pimpl->fp)
        i.raise_exc("ValueError",
                    std::string("I/O operation on closed file (") + fn + ")",
                    {});
    return f;
}

PyRef m_read(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "read");
    int64_t n = -1;
    if (a.pos.size() > 1) {
        bool ok = false;
        n = py_to_int(a.pos[1], &ok);
    }
    std::string out;
    char buf[4096];
    std::size_t got;
    while (n < 0 || static_cast<int64_t>(out.size()) < n) {
        const std::size_t want =
            n < 0 ? sizeof(buf)
                  : std::min<std::size_t>(
                        sizeof(buf),
                        static_cast<std::size_t>(n - out.size()));
        got = std::fread(buf, 1, want, f->pimpl->fp);
        out.append(buf, got);
        if (got < want)
            break;
    }
    return f->pimpl->binary ? py_bytes(out) : py_str(out);
}
PyRef m_readline(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "readline");
    std::string out;
    int c;
    while ((c = std::fgetc(f->pimpl->fp)) != EOF) {
        out += static_cast<char>(c);
        if (c == '\n')
            break;
    }
    return f->pimpl->binary ? py_bytes(out) : py_str(out);
}
PyRef m_readlines(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "readlines");
    std::vector<PyRef> lines;
    int c;
    std::string cur;
    while ((c = std::fgetc(f->pimpl->fp)) != EOF) {
        cur += static_cast<char>(c);
        if (c == '\n') {
            lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
            cur.clear();
        }
    }
    if (!cur.empty())
        lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
    return py_list(std::move(lines));
}
PyRef m_write(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "write");
    std::string data;
    if (a.pos.size() > 1) {
        if (auto* s = as_str(a.pos[1]))
            data = s->v;
        else if (auto* b = as_bytes(a.pos[1]))
            data = b->v;
        else
            data = py_to_str(i, a.pos[1]);
    }
    const std::size_t n = std::fwrite(data.data(), 1, data.size(),
                                      f->pimpl->fp);
    f->pimpl->written = true;
    return py_int(static_cast<int64_t>(n));
}
PyRef m_writelines(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "writelines");
    if (a.pos.size() > 1) {
        i.for_each(a.pos[1], [&](PyRef v) {
            std::string data;
            if (auto* s = as_str(v))
                data = s->v;
            else if (auto* b = as_bytes(v))
                data = b->v;
            else
                data = py_to_str(i, v);
            std::fwrite(data.data(), 1, data.size(), f->pimpl->fp);
            return true;
        });
    }
    return py_none();
}
PyRef m_flush(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "flush");
    std::fflush(f->pimpl->fp);
    return py_none();
}
PyRef m_seek(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "seek");
    bool ok = false;
    const int64_t off = py_to_int(arg_at(i, a, 1, "seek"), &ok);
    int64_t whence = 0;
    if (a.pos.size() > 2)
        whence = py_to_int(a.pos[2], &ok);
    const int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
#ifdef _WIN32
    _fseeki64(f->pimpl->fp, off, w);
    return py_int(_ftelli64(f->pimpl->fp));
#else
    std::fseek(f->pimpl->fp, static_cast<long>(off), w);
    return py_int(std::ftell(f->pimpl->fp));
#endif
}
PyRef m_tell(interpreter& i, const py_args& a) {
    auto* f = need_file(i, a, "tell");
#ifdef _WIN32
    return py_int(_ftelli64(f->pimpl->fp));
#else
    return py_int(std::ftell(f->pimpl->fp));
#endif
}
PyRef m_close(interpreter& i, const py_args& a) {
    auto* f = as_file(arg_at(i, a, 0, "close"));
    if (f && f->pimpl && f->pimpl->fp) {
        std::fclose(f->pimpl->fp);
        f->pimpl->fp = nullptr;
    }
    return py_none();
}
PyRef m_enter(interpreter& i, const py_args& a) { return a.pos[0]; }
PyRef m_exit(interpreter& i, const py_args& a) {
    return m_close(i, a);   // never suppresses
}
PyRef m_iter_lines(interpreter&, const py_args&);   // n/a — iter() materializes
PyRef m_seekable(interpreter& i, const py_args& a) {
    (void)a;
    return py_true();
}
PyRef m_readable(interpreter& i, const py_args& a) {
    (void)a;
    return py_true();
}
PyRef m_writable(interpreter& i, const py_args& a) {
    (void)a;
    return py_true();
}
} // namespace file_impl

// file_lines_impl — materialize remaining lines (iter() calls the extern
// file_lines wrapper below)
PyRef file_lines_impl(interpreter&, const PyRef& fobj) {
    auto* f = as_file(fobj);
    if (!f || !f->pimpl || !f->pimpl->fp)
        return py_list();
    std::vector<PyRef> lines;
    std::string cur;
    int c;
    while ((c = std::fgetc(f->pimpl->fp)) != EOF) {
        cur += static_cast<char>(c);
        if (c == '\n') {
            lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
            cur.clear();
        }
    }
    if (!cur.empty())
        lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
    return py_list(std::move(lines));
}

// open() — sandboxed to cfg.plugin_root
PyRef open_path(interpreter& i, const std::string& path,
                const std::string& mode) {
    const bool binary = mode.find('b') != std::string::npos;
    const wchar_t* wmode = L"rb";
    if (mode.find('w') != std::string::npos ||
        mode.find('a') != std::string::npos ||
        mode.find('+') != std::string::npos) {
        const char m = mode.find('w') != std::string::npos
                           ? 'w'
                           : mode.find('a') != std::string::npos ? 'a'
                                                                 : 'r';
        std::string wm;
        wm += m;
        if (mode.find('+') != std::string::npos)
            wm += '+';
        if (!binary)
            wm += 't';
        else
            wm += 'b';
        static thread_local std::wstring wms;
        wms = sb::widen(wm);
        wmode = wms.c_str();
    }
    const std::wstring full = sb::resolve_in_root(i, path);
    if (mode.find('w') != std::string::npos ||
        mode.find('a') != std::string::npos) {
        // ensure parent exists inside sandbox
        const auto sep = full.find_last_of(L"\\/");
        if (sep != std::wstring::npos)
            sb::mkdirs_w(full.substr(0, sep));
    }
    auto fo = std::make_shared<PyFileObj>();
    fo->pimpl = std::make_unique<PyFileObj::impl>();
    fo->pimpl->binary = binary;
    fo->mode = mode;
    if (mode.find('r') != std::string::npos &&
        mode.find('w') == std::string::npos &&
        mode.find('a') == std::string::npos &&
        mode.find('+') == std::string::npos) {
        wmode = binary ? L"rb" : L"rt";
    }
    fo->pimpl->fp = _wfopen(full.c_str(), wmode);
    if (!fo->pimpl->fp)
        i.raise_exc("FileNotFoundError",
                    "No such file or directory: '" + path + "'", {});
    return fo;
}

PyRef m_open(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "open"));
    std::string mode = "r";
    if (a.pos.size() > 1)
        if (auto* m = as_str(a.pos[1]))
            mode = m->v;
    if (!p)
        i.raise_exc("TypeError", "open() path must be str", {});
    return open_path(i, p->v, mode);
}

// ═══ io module ═══
PyRef mod_io(interpreter& i) {
    PyRef m = mk_mod("io");
    put_fn(m, "open", m_open);
    put_c(m, "DEFAULT_BUFFER_SIZE", py_int(8192));
    return m;
}

// ═══ os + os.path ═══
namespace os_impl {
PyRef m_getcwd(interpreter& i, const py_args& a) {
    (void)a;
    return py_str(i.cfg.plugin_root.empty()
                      ? "."
                      : sb::narrow(i.cfg.plugin_root));
}
PyRef m_getenv(interpreter& i, const py_args& a) {
    const auto* k = as_str(arg_at(i, a, 0, "getenv"));
    if (!k)
        i.raise_exc("TypeError", "getenv() key must be str", {});
    const std::wstring wk = sb::widen(k->v);
    const DWORD n = GetEnvironmentVariableW(wk.c_str(), nullptr, 0);
    if (n == 0) {
        if (a.pos.size() > 1)
            return a.pos[1];
        return py_none();
    }
    std::wstring v(n - 1, L'\0');
    GetEnvironmentVariableW(wk.c_str(), v.data(), n);
    return py_str(sb::narrow(v));
}
PyRef m_listdir(interpreter& i, const py_args& a) {
    std::string p = ".";
    if (a.pos.size() > 0)
        if (auto* s = as_str(a.pos[0]))
            p = s->v;
    const std::wstring full = sb::resolve_in_root(i, p) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(full.c_str(), &fd);
    std::vector<PyRef> out;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..")
                continue;
            out.push_back(py_str(sb::narrow(n)));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return py_list(std::move(out));
}
PyRef m_makedirs(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "makedirs"));
    if (!p)
        i.raise_exc("TypeError", "makedirs() expects str", {});
    sb::mkdirs_w(sb::resolve_in_root(i, p->v));
    return py_none();
}
PyRef m_mkdir(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "mkdir"));
    if (!p)
        i.raise_exc("TypeError", "mkdir() expects str", {});
    const std::wstring full = sb::resolve_in_root(i, p->v);
    if (!CreateDirectoryW(full.c_str(), nullptr))
        i.raise_exc("FileExistsError",
                    "cannot create dir: '" + p->v + "'", {});
    return py_none();
}
PyRef m_remove(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "remove"));
    if (!p)
        i.raise_exc("TypeError", "remove() expects str", {});
    const std::wstring full = sb::resolve_in_root(i, p->v);
    if (sb::isdir_w(full)) {
        if (!RemoveDirectoryW(full.c_str()))
            i.raise_exc("OSError", "cannot remove dir: '" + p->v + "'", {});
    } else {
        if (!DeleteFileW(full.c_str()))
            i.raise_exc("FileNotFoundError",
                        "cannot remove: '" + p->v + "'", {});
    }
    return py_none();
}
PyRef m_rename(interpreter& i, const py_args& a) {
    const auto* s = as_str(arg_at(i, a, 0, "rename"));
    const auto* d = as_str(arg_at(i, a, 1, "rename"));
    if (!s || !d)
        i.raise_exc("TypeError", "rename() expects str paths", {});
    if (!MoveFileW(sb::resolve_in_root(i, s->v).c_str(),
                   sb::resolve_in_root(i, d->v).c_str()))
        i.raise_exc("OSError", "rename failed", {});
    return py_none();
}
PyRef m_urandom(interpreter& i, const py_args& a) {
    bool ok = false;
    const int64_t n = py_to_int(arg_at(i, a, 0, "urandom"), &ok);
    std::string out(static_cast<std::size_t>(n), '\0');
    for (auto& c : out)
        c = static_cast<char>(rand());
    return py_bytes(out);
}
PyRef m_chdir(interpreter& i, const py_args& a) {
    i.raise_exc("OSError", "os.chdir is not permitted in pymini", {});
}
PyRef m_system(interpreter& i, const py_args& a) {
    i.raise_exc("OSError", "os.system is not permitted in pymini", {});
}
} // namespace os_impl

namespace ospath_impl {
std::string norm(const std::string& p) {
    std::string out = p;
    for (auto& c : out)
        if (c == '/')
            c = '\\';
    // collapse . and .. textually
    std::vector<std::string> parts;
    std::size_t k = 0;
    while (k <= out.size()) {
        const auto sep = out.find('\\', k);
        std::string seg = out.substr(k, sep == std::string::npos
                                           ? std::string::npos
                                           : sep - k);
        if (seg == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (sep == std::string::npos)
            break;
        k = sep + 1;
    }
    std::string joined;
    for (const auto& s : parts) {
        if (!joined.empty())
            joined += '\\';
        joined += s;
    }
    return joined;
}
PyRef m_join(interpreter& i, const py_args& a) {
    std::string out;
    for (const auto& p : a.pos) {
        const auto* s = as_str(p);
        std::string seg = s ? s->v : py_to_str(i, p);
        for (auto& c : seg)
            if (c == '/')
                c = '\\';
        if (!out.empty() && !seg.empty() && seg.front() != '\\' &&
            out.back() != '\\')
            out += '\\';
        out += seg;
    }
    return py_str(out);
}
PyRef m_normpath(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "normpath"));
    return py_str(norm(p->v));
}
PyRef m_basename(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "basename"));
    const auto pos = p->v.find_last_of("\\/");
    return py_str(pos == std::string::npos ? p->v : p->v.substr(pos + 1));
}
PyRef m_dirname(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "dirname"));
    const auto pos = p->v.find_last_of("\\/");
    return py_str(pos == std::string::npos ? "" : p->v.substr(0, pos));
}
PyRef m_split(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "split"));
    const auto pos = p->v.find_last_of("\\/");
    if (pos == std::string::npos)
        return py_tuple({py_str(""), py_str(p->v)});
    return py_tuple({py_str(p->v.substr(0, pos)),
                     py_str(p->v.substr(pos + 1))});
}
PyRef m_splitext(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "splitext"));
    const auto sl = p->v.find_last_of("\\/");
    const auto dot = p->v.find_last_of('.');
    if (dot == std::string::npos ||
        (sl != std::string::npos && dot < sl))
        return py_tuple({a.pos[0], py_str("")});
    return py_tuple({py_str(p->v.substr(0, dot)),
                     py_str(p->v.substr(dot))});
}
PyRef m_exists(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "exists"));
    if (!p)
        return py_false();
    return py_bool(sb::exists_w(sb::resolve_in_root(i, p->v)));
}
PyRef m_isfile(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "isfile"));
    if (!p)
        return py_false();
    const std::wstring f = sb::resolve_in_root(i, p->v);
    return py_bool(sb::exists_w(f) && !sb::isdir_w(f));
}
PyRef m_isdir(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "isdir"));
    if (!p)
        return py_false();
    return py_bool(sb::isdir_w(sb::resolve_in_root(i, p->v)));
}
PyRef m_getsize(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "getsize"));
    if (!p)
        return py_int(-1);
    return py_int(sb::size_w(sb::resolve_in_root(i, p->v)));
}
PyRef m_getmtime(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "getmtime"));
    if (!p)
        return py_int(0);
    return py_int(sb::mtime_w(sb::resolve_in_root(i, p->v)));
}
PyRef m_abspath(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "abspath"));
    if (!p)
        return py_none();
    const std::string rel = norm(p->v);
    const std::string root =
        i.cfg.plugin_root.empty() ? "." : sb::narrow(i.cfg.plugin_root);
    return py_str(root + "\\" + rel);
}
PyRef m_isabs(interpreter& i, const py_args& a) {
    const auto* p = as_str(arg_at(i, a, 0, "isabs"));
    return py_bool(p && p->v.size() >= 2 && p->v[1] == ':');
}
} // namespace ospath_impl

PyRef mod_ospath(interpreter& i) {
    PyRef m = mk_mod("os.path");
    using namespace ospath_impl;
    put_fn(m, "join", m_join);
    put_fn(m, "normpath", m_normpath);
    put_fn(m, "basename", m_basename);
    put_fn(m, "dirname", m_dirname);
    put_fn(m, "split", m_split);
    put_fn(m, "splitext", m_splitext);
    put_fn(m, "exists", m_exists);
    put_fn(m, "isfile", m_isfile);
    put_fn(m, "isdir", m_isdir);
    put_fn(m, "getsize", m_getsize);
    put_fn(m, "getmtime", m_getmtime);
    put_fn(m, "abspath", m_abspath);
    put_fn(m, "isabs", m_isabs);
    return m;
}

PyRef mod_os(interpreter& i) {
    PyRef m = mk_mod("os");
    using namespace os_impl;
    put_fn(m, "getcwd", m_getcwd);
    put_fn(m, "getenv", m_getenv);
    put_fn(m, "listdir", m_listdir);
    put_fn(m, "makedirs", m_makedirs);
    put_fn(m, "mkdir", m_mkdir);
    put_fn(m, "remove", m_remove);
    put_fn(m, "unlink", m_remove);
    put_fn(m, "rmdir", m_remove);
    put_fn(m, "rename", m_rename);
    put_fn(m, "replace", m_rename);
    put_fn(m, "urandom", m_urandom);
    put_fn(m, "chdir", m_chdir);
    put_fn(m, "system", m_system);
    put_c(m, "path", mod_ospath(i));
    put_c(m, "sep", py_str("\\"));
    put_c(m, "linesep", py_str("\r\n"));
    put_c(m, "name", py_str("nt"));
    put_c(m, "environ", [] {
        auto d = py_dict();
        wchar_t* env = GetEnvironmentStringsW();
        if (env) {
            for (wchar_t* p = env; *p;) {
                const std::wstring kv = p;
                const auto eq = kv.find(L'=');
                if (eq != std::wstring::npos)
                    dict_set(as_dict(d),
                             py_str(sb::narrow(kv.substr(0, eq))),
                             py_str(sb::narrow(kv.substr(eq + 1))));
                p += kv.size() + 1;
            }
            FreeEnvironmentStringsW(env);
        }
        return d;
    }());
    // os.getenv reads the real environment (read-only is harmless)
    return m;
}

// ═══ sys ═══
PyRef mod_sys(interpreter& i) {
    PyRef m = mk_mod("sys");
    put_c(m, "argv", py_list());
    put_c(m, "version", py_str("3.11.0 (pymini native-subset)"));
    put_c(m, "version_info",
          py_tuple({py_int(3), py_int(11), py_int(0), py_str("pymini"),
                    py_int(0)}));
    put_c(m, "platform", py_str("win32"));
    put_c(m, "exec_prefix", py_str("pymini"));
    put_c(m, "prefix", py_str("pymini"));
    put_c(m, "maxsize", py_int(INT64_MAX));
    put_c(m, "modules", i.sys_modules);
    put_c(m, "path", [&] {
        std::vector<PyRef> dirs;
        for (const auto& d : i.cfg.module_dirs)
            dirs.push_back(py_str(sb::narrow(d)));
        return py_list(std::move(dirs));
    }());
    put_c(m, "builtin_module_names", py_tuple({}));
    put_c(m, "implementation", py_dict());
    put_fn(m, "exit", [](interpreter& i2, const py_args& a) -> PyRef {
        (void)a;
        raise_system_exit(i2);   // void helper — keeps return reachable
        return py_none();
    });
    put_fn(m, "getrefcount", [](interpreter&, const py_args&) {
        return py_int(1);
    });
    put_fn(m, "setrecursionlimit", [](interpreter&, const py_args&) {
        return py_none();
    });
    put_fn(m, "getdefaultencoding", [](interpreter&, const py_args&) {
        return py_str("utf-8");
    });
    return m;
}

// ═══ threading ═══
namespace thread_impl {
PyThreadObj* need_thread(interpreter& i, const py_args& a, const char* fn) {
    auto* t = as_thread(arg_at(i, a, 0, fn));
    if (!t)
        i.raise_exc("TypeError",
                    std::string(fn) + ": expected Thread", {});
    return t;
}

PyRef m_start(interpreter& i, const py_args& a) {
    auto* t = need_thread(i, a, "start");
    if (t->pimpl->started.exchange(true))
        i.raise_exc("RuntimeError", "threads can only be started once", {});
    interpreter* ip = &i;
    PyRef target = t->pimpl->target;
    py_args args = t->pimpl->args;
    t->pimpl->th = std::thread([ip, target, args,
                              done = t->pimpl->done]() mutable {
        try {
            // each call boundary takes the gil; script runs interleaved
            ip->call(target, args, {});
        } catch (const sig_raise& sig) {
            if (ip->on_log)
                ip->on_log("[thread] unhandled exception: " +
                           py_repr(*ip, sig.exc));
        } catch (const std::exception& e) {
            if (ip->on_log)
                ip->on_log(std::string("[thread] native error: ") + e.what());
        } catch (...) {
        }
        done->store(true);
    });
    return py_none();
}
PyRef m_join(interpreter& i, const py_args& a) {
    auto* t = need_thread(i, a, "join");
    double timeout = -1;
    if (a.pos.size() > 1) {
        bool ok = false;
        timeout = py_to_float(a.pos[1], &ok);
    }
    if (t->pimpl->th.joinable()) {
        // release gil while waiting so worker can finish
        i.gil.unlock();
        struct rg {
            std::recursive_mutex* m;
            ~rg() { m->lock(); }
        } g{&i.gil};
        if (timeout < 0) {
            t->pimpl->th.join();
        } else {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int64_t>(timeout * 1000));
            while (!t->pimpl->done->load() &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (t->pimpl->done->load())
                t->pimpl->th.join();
        }
    }
    return py_none();
}
PyRef m_is_alive(interpreter& i, const py_args& a) {
    auto* t = need_thread(i, a, "is_alive");
    return py_bool(t->pimpl->started.load() && !t->pimpl->done->load());
}
PyRef m_thread_ctor(interpreter& i, const py_args& a) {
    auto t = std::make_shared<PyThreadObj>();
    t->pimpl = std::make_unique<PyThreadObj::impl>();
    PyRef target;
    for (const auto& [k, v] : a.kw) {
        if (k == "target")
            target = v;
        if (k == "name")
            if (auto* s = as_str(v))
                t->pimpl->name = s->v;
        if (k == "args") {
            // CPython expects a tuple but a list is equivalent here;
            // silently dropping list args made kwargs-only call sites no-op.
            const std::vector<PyRef>* seq = nullptr;
            if (auto* l = as_list(v))
                seq = &l->v;
            else if (auto* tp = as_tuple(v))
                seq = &tp->v;
            if (seq)
                for (const auto& x : *seq)
                    t->pimpl->args.pos.push_back(x);
        }
        if (k == "kwargs")
            if (auto* d = as_dict(v))
                for (const auto& [k2, v2] : d->items)
                    if (auto* ks = as_str(k2))
                        t->pimpl->args.kw.emplace_back(ks->v, v2);
    }
    if (!target && !a.pos.empty())
        target = a.pos[0];
    if (!target)
        i.raise_exc("TypeError", "Thread() requires target=", {});
    t->pimpl->target = target;
    if (t->pimpl->name.empty())
        t->pimpl->name = "Thread-" + std::to_string(rand());
    return t;
}
PyRef m_current_thread(interpreter& i, const py_args& a) {
    (void)a;
    auto d = py_dict();
    dict_set(as_dict(d), py_str("name"), py_str("MainThread"));
    dict_set(as_dict(d), py_str("daemon"), py_false());
    dict_set(as_dict(d), py_str("ident"), py_int(1));
    return d;
}
PyRef m_active_count(interpreter& i, const py_args& a) {
    (void)a;
    return py_int(1);   // caller-owned threads not tracked centrally (subset)
}

// Lock / RLock share PyLockObj (recursive_mutex — RLock semantics)
PyRef m_lock_ctor(interpreter& i, const py_args& a) {
    (void)i;
    (void)a;
    return std::make_shared<PyLockObj>();
}
PyRef m_lock_acquire(interpreter& i, const py_args& a) {
    auto* l = as_lock(arg_at(i, a, 0, "acquire"));
    bool blocking = true;
    double timeout = -1;
    if (a.pos.size() > 1)
        blocking = i.truthy(a.pos[1]);
    if (a.pos.size() > 2) {
        bool ok = false;
        timeout = py_to_float(a.pos[2], &ok);
    }
    if (blocking && timeout < 0) {
        l->mutex.lock();
        return py_true();
    }
    // try / timed try
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout < 0 ? 0
                                              : static_cast<int64_t>(
                                                    timeout * 1000));
    while (true) {
        if (l->mutex.try_lock())
            return py_true();
        if (std::chrono::steady_clock::now() >= deadline)
            return py_false();
        std::this_thread::yield();
    }
}
PyRef m_lock_release(interpreter& i, const py_args& a) {
    auto* l = as_lock(arg_at(i, a, 0, "release"));
    l->mutex.unlock();
    return py_none();
}
PyRef m_lock_enter(interpreter& i, const py_args& a) {
    auto* l = as_lock(arg_at(i, a, 0, "__enter__"));
    l->mutex.lock();
    return a.pos[0];
}
PyRef m_lock_exit(interpreter& i, const py_args& a) {
    auto* l = as_lock(arg_at(i, a, 0, "__exit__"));
    l->mutex.unlock();
    return py_false();
}
PyRef m_lock_locked(interpreter& i, const py_args& a) {
    auto* l = as_lock(arg_at(i, a, 0, "locked"));
    if (l->mutex.try_lock()) {
        l->mutex.unlock();
        return py_false();
    }
    return py_true();
}

// Event
PyRef m_event_ctor(interpreter& i, const py_args& a) {
    (void)i;
    (void)a;
    return std::make_shared<PyEventObj>();
}
PyRef m_event_set(interpreter& i, const py_args& a) {
    auto* e = as_event(arg_at(i, a, 0, "set"));
    {
        std::lock_guard<std::mutex> g(e->m);
        e->flag = true;
    }
    e->cv.notify_all();
    return py_none();
}
PyRef m_event_clear(interpreter& i, const py_args& a) {
    auto* e = as_event(arg_at(i, a, 0, "clear"));
    std::lock_guard<std::mutex> g(e->m);
    e->flag = false;
    return py_none();
}
PyRef m_event_is_set(interpreter& i, const py_args& a) {
    auto* e = as_event(arg_at(i, a, 0, "is_set"));
    std::lock_guard<std::mutex> g(e->m);
    return py_bool(e->flag);
}
PyRef m_event_wait(interpreter& i, const py_args& a) {
    auto* e = as_event(arg_at(i, a, 0, "wait"));
    double timeout = -1;
    if (a.pos.size() > 1) {
        bool ok = false;
        timeout = py_to_float(a.pos[1], &ok);
    }
    i.gil.unlock();
    struct rg {
        std::recursive_mutex* m;
        ~rg() { m->lock(); }
    } g{&i.gil};
    std::unique_lock<std::mutex> lk(e->m);
    if (timeout < 0) {
        e->cv.wait(lk, [&] { return e->flag; });
        return py_true();
    }
    return py_bool(e->cv.wait_for(
        lk, std::chrono::milliseconds(static_cast<int64_t>(timeout * 1000)),
        [&] { return e->flag; }));
}
} // namespace thread_impl

PyRef mod_threading(interpreter& i) {
    PyRef m = mk_mod("threading");
    using namespace thread_impl;
    put_c(m, "Thread", py_builtin("Thread", m_thread_ctor));
    put_c(m, "Lock", py_builtin("Lock", m_lock_ctor));
    put_c(m, "RLock", py_builtin("RLock", m_lock_ctor));
    put_c(m, "Event", py_builtin("Event", m_event_ctor));
    put_fn(m, "current_thread", m_current_thread);
    put_fn(m, "main_thread", m_current_thread);
    put_fn(m, "active_count", m_active_count);
    put_c(m, "TIMEOUT_MAX", py_float(4294967.0));
    return m;
}

// ═══ queue ═══
namespace queue_impl {
PyQueueObj* need_q(interpreter& i, const py_args& a, const char* fn) {
    auto* q = as_queue(arg_at(i, a, 0, fn));
    if (!q)
        i.raise_exc("TypeError",
                    std::string(fn) + ": expected Queue", {});
    return q;
}
PyRef m_queue_ctor(interpreter& i, const py_args& a) {
    auto q = std::make_shared<PyQueueObj>(false);
    if (!a.pos.empty()) {
        bool ok = false;
        q->maxsize = static_cast<std::size_t>(
            std::max<int64_t>(0, py_to_int(a.pos[0], &ok)));
    }
    return q;
}
PyRef m_lifo_ctor(interpreter& i, const py_args& a) {
    auto q = std::make_shared<PyQueueObj>(true);
    if (!a.pos.empty()) {
        bool ok = false;
        q->maxsize = static_cast<std::size_t>(
            std::max<int64_t>(0, py_to_int(a.pos[0], &ok)));
    }
    return q;
}
PyRef m_put(interpreter& i, const py_args& a) {
    auto* q = need_q(i, a, "put");
    PyRef item = arg_at(i, a, 1, "put");
    std::lock_guard<std::mutex> g(q->m);
    if (q->maxsize && q->items.size() >= q->maxsize) {
        // subset: block=False check
        if (a.pos.size() > 2 && !i.truthy(a.pos[2]))
            i.raise_exc("Full", "queue is full", {});
    }
    q->items.push_back(item);
    q->not_empty.notify_one();
    return py_none();
}
PyRef m_get(interpreter& i, const py_args& a) {
    auto* q = need_q(i, a, "get");
    bool block = true;
    double timeout = -1;
    if (a.pos.size() > 1)
        block = i.truthy(a.pos[1]);
    if (a.pos.size() > 2) {
        bool ok = false;
        timeout = py_to_float(a.pos[2], &ok);
    }
    i.gil.unlock();
    struct rg {
        std::recursive_mutex* m;
        ~rg() { m->lock(); }
    } g{&i.gil};
    std::unique_lock<std::mutex> lk(q->m);
    auto ready = [&] { return !q->items.empty(); };
    const bool ok2 =
        !block ? ready()
               : timeout < 0 ? (q->not_empty.wait(lk, ready), true)
                             : q->not_empty.wait_for(
                                   lk,
                                   std::chrono::milliseconds(
                                       static_cast<int64_t>(timeout * 1000)),
                                   ready);
    if (!ok2)
        i.raise_exc("Empty", "queue is empty", {});
    PyRef v = q->lifo ? q->items.back() : q->items.front();
    if (q->lifo)
        q->items.pop_back();
    else
        q->items.pop_front();
    return v;
}
PyRef m_put_nowait(interpreter& i, const py_args& a) {
    py_args a2 = a;
    a2.pos.resize(3);
    a2.pos[2] = py_false();
    return m_put(i, a2);
}
PyRef m_get_nowait(interpreter& i, const py_args& a) {
    py_args a2 = a;
    a2.pos.push_back(py_false());
    return m_get(i, a2);
}
PyRef m_qsize(interpreter& i, const py_args& a) {
    auto* q = need_q(i, a, "qsize");
    std::lock_guard<std::mutex> g(q->m);
    return py_int(static_cast<int64_t>(q->items.size()));
}
PyRef m_empty(interpreter& i, const py_args& a) {
    auto* q = need_q(i, a, "empty");
    std::lock_guard<std::mutex> g(q->m);
    return py_bool(q->items.empty());
}
PyRef m_full(interpreter& i, const py_args& a) {
    auto* q = need_q(i, a, "full");
    std::lock_guard<std::mutex> g(q->m);
    return py_bool(q->maxsize != 0 && q->items.size() >= q->maxsize);
}
PyRef m_task_done(interpreter&, const py_args&) { return py_none(); }
PyRef m_q_join(interpreter&, const py_args&) { return py_none(); }
} // namespace queue_impl

PyRef mod_queue(interpreter& i) {
    PyRef m = mk_mod("queue");
    using namespace queue_impl;
    put_c(m, "Queue", py_builtin("Queue", m_queue_ctor));
    put_c(m, "LifoQueue", py_builtin("LifoQueue", m_lifo_ctor));
    put_c(m, "PriorityQueue", py_builtin("PriorityQueue", m_queue_ctor));
    return m;
}

// ═══ logging ═══
namespace log_impl {
PyRef make_logger(interpreter& i, const std::string& name) {
    auto d = py_dict();
    auto logfn = [&](const char* level) {
        return py_builtin(level, [&i, level, name](interpreter&,
                                                 const py_args& a) {
            std::string msg;
            for (const auto& p : a.pos) {
                if (!msg.empty())
                    msg += ' ';
                msg += py_to_str(i, p);
            }
            if (i.on_log)
                i.on_log("[" + std::string(level) + ":" + name + "] " + msg);
            return py_none();
        });
    };
    dict_set(as_dict(d), py_str("debug"), logfn("debug"));
    dict_set(as_dict(d), py_str("info"), logfn("info"));
    dict_set(as_dict(d), py_str("warning"), logfn("warning"));
    dict_set(as_dict(d), py_str("warn"), logfn("warning"));
    dict_set(as_dict(d), py_str("error"), logfn("error"));
    dict_set(as_dict(d), py_str("critical"), logfn("critical"));
    dict_set(as_dict(d), py_str("exception"), logfn("error"));
    dict_set(as_dict(d), py_str("name"), py_str(name));
    dict_set(as_dict(d), py_str("level"), py_int(20));
    dict_set(as_dict(d), py_str("setLevel"),
             py_builtin("setLevel", [](interpreter&, const py_args&) {
                 return py_none();
             }));
    dict_set(as_dict(d), py_str("addHandler"),
             py_builtin("addHandler", [](interpreter&, const py_args&) {
                 return py_none();
             }));
    dict_set(as_dict(d), py_str("handlers"), py_list());
    return d;
}
PyRef m_getlogger(interpreter& i, const py_args& a) {
    std::string name = "root";
    if (!a.pos.empty())
        if (auto* s = as_str(a.pos[0]))
            name = s->v;
    return make_logger(i, name);
}
PyRef m_basicconfig(interpreter&, const py_args&) { return py_none(); }
PyRef m_level_call(interpreter& i, const py_args& a) {
    return make_logger(i, "root");
}
} // namespace log_impl

PyRef mod_logging(interpreter& i) {
    PyRef m = mk_mod("logging");
    put_fn(m, "getLogger", log_impl::m_getlogger);
    put_fn(m, "basicConfig", log_impl::m_basicconfig);
    put_c(m, "DEBUG", py_int(10));
    put_c(m, "INFO", py_int(20));
    put_c(m, "WARNING", py_int(30));
    put_c(m, "ERROR", py_int(40));
    put_c(m, "CRITICAL", py_int(50));
    return m;
}

// ═══ copy ═══
namespace copy_impl {
PyRef clone(interpreter& i, const PyRef& v, bool deep,
            std::unordered_map<const PyObj*, PyRef>& memo) {
    if (!v)
        return v;
    auto it = memo.find(v.get());
    if (it != memo.end())
        return it->second;
    switch (v->kind) {
    case py_kind::list: {
        auto out = py_list();
        memo[v.get()] = out;
        auto* l = as_list(out);
        for (const auto& x : as_list(v)->v)
            l->v.push_back(deep ? clone(i, x, true, memo) : x);
        return out;
    }
    case py_kind::tuple_: {
        std::vector<PyRef> items;
        for (const auto& x : as_tuple(v)->v)
            items.push_back(deep ? clone(i, x, true, memo) : x);
        auto out = py_tuple(std::move(items));
        memo[v.get()] = out;
        return out;
    }
    case py_kind::set:
    case py_kind::frozenset: {
        auto out = py_set();
        memo[v.get()] = out;
        auto* s = as_set(out);
        for (const auto& x : as_set(v)->items)
            s->items.push_back(deep ? clone(i, x, true, memo) : x);
        return out;
    }
    case py_kind::dict: {
        auto out = py_dict();
        memo[v.get()] = out;
        for (const auto& [k, x] : as_dict(v)->items)
            dict_set(i, as_dict(out), deep ? clone(i, k, true, memo) : k,
                     deep ? clone(i, x, true, memo) : x);
        return out;
    }
    case py_kind::instance: {
        // shallow: fresh instance sharing klass, copied attrs dict
        auto* in = as_inst(v);
        auto out = std::make_shared<PyInstanceObj>(in->klass, py_dict());
        for (const auto& [k, x] : as_dict(in->attrs)->items)
            dict_set(as_dict(out->attrs), k,
                     deep ? clone(i, x, true, memo) : x);
        memo[v.get()] = out;
        return out;
    }
    default:
        return v;   // scalars share fine
    }
}
PyRef m_copy(interpreter& i, const py_args& a) {
    std::unordered_map<const PyObj*, PyRef> memo;
    return clone(i, arg_at(i, a, 0, "copy"), false, memo);
}
PyRef m_deepcopy(interpreter& i, const py_args& a) {
    std::unordered_map<const PyObj*, PyRef> memo;
    return clone(i, arg_at(i, a, 0, "deepcopy"), true, memo);
}
} // namespace copy_impl

PyRef mod_copy(interpreter& i) {
    PyRef m = mk_mod("copy");
    put_fn(m, "copy", copy_impl::m_copy);
    put_fn(m, "deepcopy", copy_impl::m_deepcopy);
    return m;
}

// ═══ traceback ═══
namespace tb_impl {
PyRef m_format_exc(interpreter& i, const py_args& a) {
    (void)a;
    std::string out = "Traceback (most recent call last):\n";
    if (auto* e = as_exc(i.active_exc)) {
        for (const auto& t : e->trace)
            out += "  " + t + "\n";
        std::string msg;
        if (auto* t = as_tuple(e->args); t && !t->v.empty())
            msg = py_to_str(i, t->v[0]);
        out += e->type_name + (msg.empty() ? "" : ": " + msg) + "\n";
    }
    return py_str(out);
}
PyRef m_print_exc(interpreter& i, const py_args& a) {
    PyRef s = m_format_exc(i, a);
    if (i.on_log)
        i.on_log(as_str(s)->v);
    return py_none();
}
PyRef m_extract_stack(interpreter&, const py_args&) { return py_list(); }
} // namespace tb_impl

PyRef mod_traceback(interpreter& i) {
    PyRef m = mk_mod("traceback");
    put_fn(m, "format_exc", tb_impl::m_format_exc);
    put_fn(m, "print_exc", tb_impl::m_print_exc);
    put_fn(m, "extract_stack", tb_impl::m_extract_stack);
    return m;
}

// ═══ warnings / abc / typing / contextlib ═══
PyRef mod_warnings(interpreter& i) {
    PyRef m = mk_mod("warnings");
    put_fn(m, "warn", [](interpreter& i2, const py_args& a) {
        std::string msg = a.pos.empty() ? "" : py_to_str(i2, a.pos[0]);
        if (i2.on_log)
            i2.on_log("warning: " + msg);
        return py_none();
    });
    put_fn(m, "simplefilter", [](interpreter&, const py_args&) {
        return py_none();
    });
    put_fn(m, "filterwarnings", [](interpreter&, const py_args&) {
        return py_none();
    });
    return m;
}

PyRef mod_abc(interpreter& i) {
    PyRef m = mk_mod("abc");
    put_c(m, "ABCMeta", dict_get(as_dict(i.builtins_dict), py_str("type")));
    put_c(m, "abstractmethod",
          py_builtin("abstractmethod", [](interpreter&, const py_args& a) {
              return a.pos.empty() ? py_none() : a.pos[0];
          }));
    put_c(m, "abstractproperty",
          py_builtin("abstractproperty", [](interpreter&, const py_args& a) {
              return a.pos.empty() ? py_none() : a.pos[0];
          }));
    put_c(m, "abstractclassmethod",
          py_builtin("abstractclassmethod", [](interpreter&, const py_args& a) {
              return a.pos.empty() ? py_none() : a.pos[0];
          }));
    put_c(m, "abstractstaticmethod",
          py_builtin("abstractstaticmethod", [](interpreter&, const py_args& a) {
              return a.pos.empty() ? py_none() : a.pos[0];
          }));
    put_c(m, "ABC", dict_get(as_dict(i.builtins_dict), py_str("object")));
    return m;
}

PyRef mod_typing(interpreter& i) {
    PyRef m = mk_mod("typing");
    // generic aliases: class _G with __getitem__ returning self — subscript
    // `List[int]` resolves to the same no-op alias.
    auto gcls = std::make_shared<PyClassObj>();
    gcls->name = "_Generic";
    gcls->attrs = py_dict();
    dict_set(as_dict(gcls->attrs), py_str("__getitem__"),
             py_builtin("__getitem__", [](interpreter&, const py_args& a) {
                 return a.pos.empty() ? py_none() : a.pos[0];
             }));
    dict_set(as_dict(gcls->attrs), py_str("__or__"),
             py_builtin("__or__", [](interpreter&, const py_args& a) {
                 return a.pos.empty() ? py_none() : a.pos[0];
             }));
    dict_set(as_dict(gcls->attrs), py_str("__ror__"),
             py_builtin("__ror__", [](interpreter&, const py_args& a) {
                 return a.pos.empty() ? py_none() : a.pos[0];
             }));
    gcls->mro = {gcls.get()};
    PyRef gcls_ref = gcls;
    auto mk_alias = [&](const std::string& n) {
        auto inst = std::make_shared<PyInstanceObj>(gcls_ref, py_dict());
        dict_set(as_dict(inst->attrs), py_str("__name__"), py_str(n));
        return static_cast<PyRef>(inst);
    };
    for (const char* n :
         {"Any", "Optional", "List", "Dict", "Tuple", "Set", "Callable",
          "Iterable", "Iterator", "Sequence", "Mapping", "MutableMapping",
          "MutableSequence", "Union", "TypeVar", "Generic", "NamedTuple",
          "AnyStr", "Type", "Text", "IO", "BinaryIO", "TextIO", "NoReturn",
          "Literal", "Protocol", "TypeAlias", "Self"})
        put_c(m, n, mk_alias(n));
    // TypeVar("T") / cast(x, T) / NewType(n, base)
    put_fn(m, "TypeVar", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    put_fn(m, "cast", [](interpreter&, const py_args& a) {
        return a.pos.size() > 1 ? a.pos[1] : py_none();
    });
    put_fn(m, "NewType", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    put_fn(m, "overload", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    put_fn(m, "runtime_checkable", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    put_fn(m, "get_type_hints", [](interpreter&, const py_args&) {
        return py_dict();
    });
    return m;
}

PyRef mod_contextlib(interpreter& i) {
    PyRef m = mk_mod("contextlib");
    put_fn(m, "contextmanager", [](interpreter&, const py_args& a) {
        // generator-based contextmanagers unsupported — pass fn through;
        // real plugin usage rare.
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    put_c(m, "suppress",
          py_builtin("suppress", [](interpreter&, const py_args&) {
              auto d = py_dict();
              dict_set(as_dict(d), py_str("__enter__"),
                       py_builtin("__enter__",
                                  [](interpreter&, const py_args& a) {
                                      return a.pos[0];
                                  }));
              dict_set(as_dict(d), py_str("__exit__"),
                       py_builtin("__exit__",
                                  [](interpreter&, const py_args&) {
                                      return py_true();
                                  }));
              return d;
          }));
    put_fn(m, "redirect_stdout", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    return m;
}

// ═══ bisect / heapq ═══
namespace sorted_impl {
PyRef m_bisect_left(interpreter& i, const py_args& a) {
    auto* l = as_list(arg_at(i, a, 0, "bisect_left"));
    PyRef x = arg_at(i, a, 1, "bisect_left");
    int64_t lo = 0, hi = l ? static_cast<int64_t>(l->v.size()) : 0;
    if (a.pos.size() > 3) {
        bool ok = false;
        lo = py_to_int(a.pos[2], &ok);
        hi = py_to_int(a.pos[3], &ok);
    }
    while (lo < hi) {
        const int64_t mid = (lo + hi) / 2;
        bool ok = false;
        if (py_cmp(i, l->v[static_cast<std::size_t>(mid)], x, &ok) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return py_int(lo);
}
PyRef m_bisect_right(interpreter& i, const py_args& a) {
    auto* l = as_list(arg_at(i, a, 0, "bisect_right"));
    PyRef x = arg_at(i, a, 1, "bisect_right");
    int64_t lo = 0, hi = l ? static_cast<int64_t>(l->v.size()) : 0;
    while (lo < hi) {
        const int64_t mid = (lo + hi) / 2;
        bool ok = false;
        if (py_cmp(i, x, l->v[static_cast<std::size_t>(mid)], &ok) < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return py_int(lo);
}
PyRef m_insort(interpreter& i, const py_args& a) {
    PyRef pos = m_bisect_right(i, a);
    auto* l = as_list(a.pos[0]);
    l->v.insert(l->v.begin() + as_int(pos)->v, a.pos[1]);
    return py_none();
}
} // namespace sorted_impl

PyRef mod_bisect(interpreter& i) {
    PyRef m = mk_mod("bisect");
    put_fn(m, "bisect_left", sorted_impl::m_bisect_left);
    put_fn(m, "bisect_right", sorted_impl::m_bisect_right);
    put_fn(m, "bisect", sorted_impl::m_bisect_right);
    put_fn(m, "insort", sorted_impl::m_insort);
    put_fn(m, "insort_right", sorted_impl::m_insort);
    return m;
}

PyRef mod_heapq(interpreter& i) {
    PyRef m = mk_mod("heapq");
    put_fn(m, "heappush", [](interpreter& i2, const py_args& a) {
        auto* l = as_list(arg_at(i2, a, 0, "heappush"));
        PyRef x = arg_at(i2, a, 1, "heappush");
        l->v.push_back(x);
        std::push_heap(l->v.begin(), l->v.end(),
                       [&](const PyRef& u, const PyRef& v) {
                           bool ok = false;
                           return py_cmp(i2, u, v, &ok) > 0;   // min-heap
                       });
        return py_none();
    });
    put_fn(m, "heappop", [](interpreter& i2, const py_args& a) {
        auto* l = as_list(arg_at(i2, a, 0, "heappop"));
        std::pop_heap(l->v.begin(), l->v.end(),
                      [&](const PyRef& u, const PyRef& v) {
                          bool ok = false;
                          return py_cmp(i2, u, v, &ok) > 0;
                      });
        PyRef v = l->v.back();
        l->v.pop_back();
        return v;
    });
    put_fn(m, "heapify", [](interpreter& i2, const py_args& a) {
        auto* l = as_list(arg_at(i2, a, 0, "heapify"));
        std::make_heap(l->v.begin(), l->v.end(),
                       [&](const PyRef& u, const PyRef& v) {
                           bool ok = false;
                           return py_cmp(i2, u, v, &ok) > 0;
                       });
        return py_none();
    });
    put_fn(m, "nlargest", [](interpreter& i2, const py_args& a) {
        bool ok = false;
        const int64_t n = py_to_int(arg_at(i2, a, 0, "nlargest"), &ok);
        std::vector<PyRef> items;
        i2.for_each(a.pos[1], [&](PyRef v) {
            items.push_back(v);
            return true;
        });
        std::sort(items.begin(), items.end(), [&](const PyRef& u,
                                                  const PyRef& v) {
            bool ok2 = false;
            return py_cmp(i2, u, v, &ok2) > 0;
        });
        if (n < static_cast<int64_t>(items.size()))
            items.resize(static_cast<std::size_t>(n));
        return py_list(std::move(items));
    });
    put_fn(m, "nsmallest", [](interpreter& i2, const py_args& a) {
        bool ok = false;
        const int64_t n = py_to_int(arg_at(i2, a, 0, "nsmallest"), &ok);
        std::vector<PyRef> items;
        i2.for_each(a.pos[1], [&](PyRef v) {
            items.push_back(v);
            return true;
        });
        std::sort(items.begin(), items.end(), [&](const PyRef& u,
                                                  const PyRef& v) {
            bool ok2 = false;
            return py_cmp(i2, u, v, &ok2) < 0;
        });
        if (n < static_cast<int64_t>(items.size()))
            items.resize(static_cast<std::size_t>(n));
        return py_list(std::move(items));
    });
    return m;
}

// ═══ itertools (eager list results — subset) ═══
namespace itertools_impl {
PyRef m_chain(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    for (const auto& it : a.pos)
        i.for_each(it, [&](PyRef v) {
            out.push_back(v);
            return true;
        });
    return py_list(std::move(out));
}
PyRef m_count(interpreter& i, const py_args& a) {
    int64_t start = 0, step = 1;
    if (!a.pos.empty()) {
        bool ok = false;
        start = py_to_int(a.pos[0], &ok);
        if (a.pos.size() > 1)
            step = py_to_int(a.pos[1], &ok);
    }
    // eager cap — infinite iterators return a bounded view (subset: 1e6)
    std::vector<PyRef> out;
    for (int64_t k = 0; k < 1000000; ++k)
        out.push_back(py_int(start + k * step));
    return py_list(std::move(out));
}
PyRef m_cycle(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    i.for_each(arg_at(i, a, 0, "cycle"), [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    // subset: one pass (callers usually take().next on first n)
    return py_list(std::move(items));
}
PyRef m_repeat(interpreter& i, const py_args& a) {
    PyRef v = arg_at(i, a, 0, "repeat");
    int64_t n = a.pos.size() > 1 ? [&] {
        bool ok = false;
        return py_to_int(a.pos[1], &ok);
    }()
                                 : 5;
    return py_list(std::vector<PyRef>(static_cast<std::size_t>(n), v));
}
PyRef m_takewhile(interpreter& i, const py_args& a) {
    PyRef pred = arg_at(i, a, 0, "takewhile");
    std::vector<PyRef> out;
    i.for_each(arg_at(i, a, 1, "takewhile"), [&](PyRef v) {
        PyRef r = i.call1(pred, v, {});
        if (!i.truthy(r))
            return false;
        out.push_back(v);
        return true;
    });
    return py_list(std::move(out));
}
PyRef m_dropwhile(interpreter& i, const py_args& a) {
    PyRef pred = arg_at(i, a, 0, "dropwhile");
    std::vector<PyRef> out;
    bool dropping = true;
    i.for_each(arg_at(i, a, 1, "dropwhile"), [&](PyRef v) {
        if (dropping) {
            PyRef r = i.call1(pred, v, {});
            if (i.truthy(r))
                return true;
            dropping = false;
        }
        out.push_back(v);
        return true;
    });
    return py_list(std::move(out));
}
PyRef m_product(interpreter& i, const py_args& a) {
    std::vector<std::vector<PyRef>> pools;
    for (const auto& p : a.pos) {
        std::vector<PyRef> items;
        i.for_each(p, [&](PyRef v) {
            items.push_back(v);
            return true;
        });
        pools.push_back(std::move(items));
    }
    std::vector<PyRef> out;
    std::vector<std::size_t> idx(pools.size(), 0);
    while (!pools.empty()) {
        std::vector<PyRef> t;
        for (std::size_t k = 0; k < pools.size(); ++k)
            t.push_back(pools[k][idx[k]]);
        out.push_back(py_tuple(std::move(t)));
        std::size_t k = pools.size();
        while (k-- > 0) {
            if (++idx[k] < pools[k].size())
                break;
            idx[k] = 0;
            if (k == 0)
                return py_list(std::move(out));
        }
    }
    return py_list(std::move(out));
}
PyRef m_combinations(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    i.for_each(arg_at(i, a, 0, "combinations"), [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    bool ok = false;
    const int64_t r = py_to_int(arg_at(i, a, 1, "combinations"), &ok);
    std::vector<PyRef> out;
    const int64_t n = static_cast<int64_t>(items.size());
    std::vector<int64_t> sel(static_cast<std::size_t>(r));
    for (int64_t k = 0; k < r; ++k)
        sel[static_cast<std::size_t>(k)] = k;
    while (r > 0 && n >= r) {
        std::vector<PyRef> t;
        for (int64_t k = 0; k < r; ++k)
            t.push_back(items[static_cast<std::size_t>(
                sel[static_cast<std::size_t>(k)])]);
        out.push_back(py_tuple(std::move(t)));
        int64_t k = r - 1;
        while (k >= 0 && sel[static_cast<std::size_t>(k)] == n - r + k)
            --k;
        if (k < 0)
            break;
        ++sel[static_cast<std::size_t>(k)];
        for (int64_t j = k + 1; j < r; ++j)
            sel[static_cast<std::size_t>(j)] =
                sel[static_cast<std::size_t>(j - 1)] + 1;
    }
    return py_list(std::move(out));
}
PyRef m_permutations(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    i.for_each(arg_at(i, a, 0, "permutations"), [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    std::vector<PyRef> out;
    std::sort(items.begin(), items.end(), [&](const PyRef& u,
                                              const PyRef& v) {
        bool ok = false;
        return py_cmp(i, u, v, &ok) < 0;
    });
    do {
        out.push_back(py_tuple(items));
    } while (std::next_permutation(items.begin(), items.end(),
                                   [&](const PyRef& u, const PyRef& v) {
                                       bool ok = false;
                                       return py_cmp(i, u, v, &ok) < 0;
                                   }));
    return py_list(std::move(out));
}
PyRef m_accumulate(interpreter& i, const py_args& a) {
    std::vector<PyRef> out;
    PyRef acc;
    bool first = true;
    i.for_each(arg_at(i, a, 0, "accumulate"), [&](PyRef v) {
        if (first) {
            acc = v;
            first = false;
        } else {
            acc = i.binary(tok_kind::plus, acc, v, {});
        }
        out.push_back(acc);
        return true;
    });
    return py_list(std::move(out));
}
PyRef m_groupby(interpreter& i, const py_args& a) {
    PyRef keyfn;
    if (a.pos.size() > 1)
        keyfn = a.pos[1];
    std::vector<PyRef> out;
    PyRef cur_key;
    std::vector<PyRef> cur_group;
    bool has_key = false;
    i.for_each(arg_at(i, a, 0, "groupby"), [&](PyRef v) {
        PyRef k = keyfn ? i.call1(keyfn, v, {}) : v;
        if (!has_key || !py_eq(i, k, cur_key)) {
            if (has_key)
                out.push_back(py_tuple(
                    {cur_key, py_list(std::move(cur_group))}));
            cur_key = k;
            cur_group.clear();
            has_key = true;
        }
        cur_group.push_back(v);
        return true;
    });
    if (has_key)
        out.push_back(py_tuple({cur_key, py_list(std::move(cur_group))}));
    return py_list(std::move(out));
}
PyRef m_islice(interpreter& i, const py_args& a) {
    std::vector<PyRef> items;
    i.for_each(arg_at(i, a, 0, "islice"), [&](PyRef v) {
        items.push_back(v);
        return true;
    });
    int64_t start = 0, stop = static_cast<int64_t>(items.size()), step = 1;
    if (a.pos.size() == 2) {
        bool ok = false;
        stop = py_to_int(a.pos[1], &ok);
    } else if (a.pos.size() >= 3) {
        bool ok = false;
        start = py_to_int(a.pos[1], &ok);
        stop = py_to_int(a.pos[2], &ok);
        if (a.pos.size() > 3)
            step = py_to_int(a.pos[3], &ok);
    }
    std::vector<PyRef> out;
    for (int64_t k = start; k < stop && k < static_cast<int64_t>(items.size());
         k += step)
        out.push_back(items[static_cast<std::size_t>(k)]);
    return py_list(std::move(out));
}
PyRef m_starmap(interpreter& i, const py_args& a) {
    PyRef fn = arg_at(i, a, 0, "starmap");
    std::vector<PyRef> out;
    i.for_each(arg_at(i, a, 1, "starmap"), [&](PyRef tup) {
        py_args ca;
        if (auto* t = as_tuple(tup))
            ca.pos = t->v;
        else if (auto* l = as_list(tup))
            ca.pos = l->v;
        out.push_back(i.call(fn, ca, {}));
        return true;
    });
    return py_list(std::move(out));
}
PyRef m_zip_longest(interpreter& i, const py_args& a) {
    std::vector<std::vector<PyRef>> cols;
    std::size_t maxn = 0;
    for (const auto& p : a.pos) {
        std::vector<PyRef> items;
        i.for_each(p, [&](PyRef v) {
            items.push_back(v);
            return true;
        });
        maxn = std::max(maxn, items.size());
        cols.push_back(std::move(items));
    }
    PyRef fill = py_none();
    for (const auto& [k, v] : a.kw)
        if (k == "fillvalue")
            fill = v;
    std::vector<PyRef> out;
    for (std::size_t r = 0; r < maxn; ++r) {
        std::vector<PyRef> t;
        for (const auto& col : cols)
            t.push_back(r < col.size() ? col[r] : fill);
        out.push_back(py_tuple(std::move(t)));
    }
    return py_list(std::move(out));
}
} // namespace itertools_impl

PyRef mod_itertools(interpreter& i) {
    PyRef m = mk_mod("itertools");
    using namespace itertools_impl;
    put_fn(m, "chain", m_chain);
    put_fn(m, "count", m_count);
    put_fn(m, "cycle", m_cycle);
    put_fn(m, "repeat", m_repeat);
    put_fn(m, "takewhile", m_takewhile);
    put_fn(m, "dropwhile", m_dropwhile);
    put_fn(m, "product", m_product);
    put_fn(m, "combinations", m_combinations);
    put_fn(m, "permutations", m_permutations);
    put_fn(m, "accumulate", m_accumulate);
    put_fn(m, "groupby", m_groupby);
    put_fn(m, "islice", m_islice);
    put_fn(m, "starmap", m_starmap);
    put_fn(m, "zip_longest", m_zip_longest);
    return m;
}

// ═══ functools ═══
namespace functools_impl {
PyRef m_partial(interpreter& i, const py_args& a) {
    PyRef fn = arg_at(i, a, 0, "partial");
    std::vector<PyRef> saved(a.pos.begin() + 1, a.pos.end());
    auto saved_kw = a.kw;
    return py_builtin("partial", [fn, saved, saved_kw](interpreter& i2,
                                                     const py_args& a2) {
        py_args ca;
        ca.pos = saved;
        ca.pos.insert(ca.pos.end(), a2.pos.begin(), a2.pos.end());
        ca.kw = saved_kw;
        ca.kw.insert(ca.kw.end(), a2.kw.begin(), a2.kw.end());
        return i2.call(fn, ca, {});
    });
}
PyRef m_reduce(interpreter& i, const py_args& a) {
    PyRef fn = arg_at(i, a, 0, "reduce");
    PyRef acc;
    bool have_acc = a.pos.size() > 2;
    if (have_acc)
        acc = a.pos[2];
    i.for_each(arg_at(i, a, 1, "reduce"), [&](PyRef v) {
        if (!have_acc) {
            acc = v;
            have_acc = true;
            return true;
        }
        py_args ca;
        ca.pos = {acc, v};
        acc = i.call(fn, ca, {});
        return true;
    });
    if (!have_acc)
        i.raise_exc("TypeError", "reduce() of empty iterable", {});
    return acc;
}
PyRef m_cache(interpreter& i, const py_args& a) {
    // lru_cache/cache decorator — memoize on repr of args (subset)
    PyRef fn = arg_at(i, a, 0, "cache");
    auto memo = std::make_shared<
        std::unordered_map<std::string, PyRef>>();
    return py_builtin("cache", [fn, memo](interpreter& i2, const py_args& a2) {
        std::string key;
        for (const auto& p : a2.pos)
            key += py_repr(i2, p) + "|";
        for (const auto& [k, v] : a2.kw)
            key += k + "=" + py_repr(i2, v) + "|";
        auto it = memo->find(key);
        if (it != memo->end())
            return it->second;
        PyRef r = i2.call(fn, a2, {});
        (*memo)[key] = r;
        return r;
    });
}
PyRef m_lru_cache(interpreter& i, const py_args& a) {
    // can be used bare or with (maxsize=None)
    if (!a.pos.empty() && as_func(a.pos[0])) {
        py_args a2;
        a2.pos.push_back(a.pos[0]);
        return m_cache(i, a2);
    }
    // decorator factory
    PyRef maxsize = a.pos.empty() ? py_none() : a.pos[0];
    for (const auto& [k, v] : a.kw)
        if (k == "maxsize")
            maxsize = v;
    return py_builtin("lru_cache", [maxsize](interpreter& i2,
                                             const py_args& a2) {
        (void)i2;
        (void)maxsize;
        auto memo = std::make_shared<
            std::unordered_map<std::string, PyRef>>();
        PyRef fn = a2.pos[0];
        return py_builtin("lru_cache", [fn, memo](interpreter& i3,
                                                  const py_args& a4) {
            std::string key;
            for (const auto& p : a4.pos)
                key += py_repr(i3, p) + "|";
            auto it = memo->find(key);
            if (it != memo->end())
                return it->second;
            PyRef r = i3.call(fn, a4, {});
            (*memo)[key] = r;
            return r;
        });
    });
}
PyRef m_wraps(interpreter& i, const py_args& a) {
    PyRef fn = arg_at(i, a, 0, "wraps");
    return py_builtin("wraps", [fn](interpreter&, const py_args& a2) {
        return a2.pos.empty() ? fn : a2.pos[0];
    });
}
PyRef m_total_ordering(interpreter& i, const py_args& a) {
    (void)i;
    return a.pos.empty() ? py_none() : a.pos[0];
}
} // namespace functools_impl

PyRef mod_functools(interpreter& i) {
    PyRef m = mk_mod("functools");
    using namespace functools_impl;
    put_fn(m, "partial", m_partial);
    put_fn(m, "reduce", m_reduce);
    put_fn(m, "cache", m_cache);
    put_fn(m, "lru_cache", m_lru_cache);
    put_fn(m, "wraps", m_wraps);
    put_fn(m, "total_ordering", m_total_ordering);
    put_fn(m, "cmp_to_key", [](interpreter&, const py_args& a) {
        return a.pos.empty() ? py_none() : a.pos[0];
    });
    return m;
}

// ═══ bootstrap python modules ═══
PyRef boot(interpreter& i, const std::string& name,
           const std::string& source) {
    PyRef mod = i.exec_module_source(name, "<stdlib-pymini>/" + name,
                                     source);
    i.register_module(mod);
    return mod;
}

const char* src_datetime = R"PY(
import time as _time

class timedelta:
    def __init__(self, days=0, seconds=0, microseconds=0, milliseconds=0,
                 minutes=0, hours=0, weeks=0):
        self.days = days + weeks * 7
        self.seconds = seconds + minutes * 60 + hours * 3600 + \
            milliseconds // 1000
        self.microseconds = microseconds + (milliseconds % 1000) * 1000
    def total_seconds(self):
        return self.days * 86400 + self.seconds + self.microseconds / 1e6

class date:
    def __init__(self, year, month=1, day=1):
        self.year = year
        self.month = month
        self.day = day
    def __str__(self):
        return "%04d-%02d-%02d" % (self.year, self.month, self.day)
    def isoformat(self):
        return str(self)
    def __repr__(self):
        return "datetime.date(%r, %r, %r)" % (self.year, self.month, self.day)

class datetime(date):
    def __init__(self, year, month=1, day=1, hour=0, minute=0, second=0,
                 microsecond=0):
        self.year = year
        self.month = month
        self.day = day
        self.hour = hour
        self.minute = minute
        self.second = second
        self.microsecond = microsecond
    @staticmethod
    def now():
        t = _time.localtime()
        d = datetime(t["tm_year"], t["tm_mon"], t["tm_mday"],
                     t["tm_hour"], t["tm_min"], t["tm_sec"])
        d._ts = _time.time()
        return d
    @staticmethod
    def utcnow():
        t = _time.gmtime()
        d = datetime(t["tm_year"], t["tm_mon"], t["tm_mday"],
                     t["tm_hour"], t["tm_min"], t["tm_sec"])
        return d
    @staticmethod
    def fromtimestamp(ts):
        t = _time.localtime(ts)
        d = datetime(t["tm_year"], t["tm_mon"], t["tm_mday"],
                     t["tm_hour"], t["tm_min"], t["tm_sec"])
        d._ts = ts
        return d
    def strftime(self, fmt):
        t = {"tm_year": self.year, "tm_mon": self.month,
             "tm_mday": self.day, "tm_hour": self.hour,
             "tm_min": self.minute, "tm_sec": self.second,
             "tm_wday": 0, "tm_yday": 1, "tm_isdst": -1}
        return _time.strftime(fmt, t)
    def isoformat(self):
        return "%04d-%02d-%02dT%02d:%02d:%02d" % (
            self.year, self.month, self.day, self.hour, self.minute,
            self.second)
    def timestamp(self):
        return self.__dict__.get("_ts", 0.0)
    def __str__(self):
        return "%04d-%02d-%02d %02d:%02d:%02d" % (
            self.year, self.month, self.day, self.hour, self.minute,
            self.second)
    def __repr__(self):
        return "datetime.datetime(%s)" % str(self)
    def date(self):
        return date(self.year, self.month, self.day)

def _install():
    datetime.min = datetime(1, 1, 1)
    datetime.max = datetime(9999, 12, 31, 23, 59, 59)
_install()
)PY";

const char* src_enum = R"PY(
# enum-lite: members become instances with .value/.name; class attrs resolve
# directly (class-subscript Color['X'] unsupported in subset).
class Enum:
    def __init_subclass__(cls):
        members = []
        mapping = {}
        for name in list(cls.__dict__.keys()):
            if name.startswith("_"):
                continue
            v = cls.__dict__[name]
            if isinstance(v, (staticmethod, classmethod, property)):
                continue
            if callable(v) and not isinstance(v, int):
                continue
            if not isinstance(v, (int, str, float, tuple)):
                continue
            # blank-instantiate: swap out __init__ so arbitrary ctors
            # don't block member creation
            saved_init = cls.__dict__.get("__init__", None)
            had_init = saved_init is not None
            cls.__init__ = lambda self: None
            try:
                inst = cls()
            except Exception:
                inst = cls.__dict__.get(name, v)
            finally:
                if had_init:
                    cls.__init__ = saved_init
                else:
                    try:
                        del cls.__dict__["__init__"]
                    except Exception:
                        pass
            if not hasattr(inst, "__class__") or inst is v:
                continue
            inst.value = v
            inst.name = name
            setattr(cls, name, inst)
            members.append(inst)
            try:
                mapping[v] = inst
            except Exception:
                pass        # unhashable value
        cls._member_names_ = members
        cls._value2member_map_ = mapping
    def __repr__(self):
        return "<" + type(self).__name__ + "." + self.__dict__.get(
            "name", "?") + ": " + repr(self.__dict__.get("value")) + ">"
    def __str__(self):
        return self.__dict__.get("name", "?")

class IntEnum(Enum):
    pass

class Flag(Enum):
    pass

class IntFlag(Flag):
    pass

_auto_cnt = [0]
def auto():
    _auto_cnt[0] += 1
    return _auto_cnt[0]
)PY";

const char* src_collections = R"PY(
class Counter:
    def __init__(self, iterable=None, **kw):
        self._c = {}
        if iterable is not None:
            self.update(iterable)
        for k, v in kw.items():
            self._c[k] = v
    def update(self, iterable):
        if isinstance(iterable, dict):
            seq = iterable.items()
        elif hasattr(iterable, "_c"):
            seq = iterable._c.items()
        else:
            seq = [(x, None) for x in iterable]
        for k, v in seq:
            if v is None:
                self._c[k] = self._c.get(k, 0) + 1
            else:
                self._c[k] = self._c.get(k, 0) + v
    def most_common(self, n=None):
        items = list(self._c.items())
        items.sort(key=lambda kv: -kv[1])
        if n is not None:
            items = items[:n]
        return items
    def elements(self):
        out = []
        for k, v in self._c.items():
            for _ in range(v):
                out.append(k)
        return out
    def total(self):
        return sum(self._c.values())
    def subtract(self, iterable):
        for x in iterable:
            self._c[x] = self._c.get(x, 0) - 1
    def get(self, k, d=None):
        return self._c.get(k, d)
    def __getitem__(self, k):
        return self._c.get(k, 0)
    def __setitem__(self, k, v):
        self._c[k] = v
    def __delitem__(self, k):
        del self._c[k]
    def __contains__(self, k):
        return k in self._c
    def __iter__(self):
        return iter(self._c)
    def __len__(self):
        return len(self._c)
    def items(self):
        return self._c.items()
    def keys(self):
        return self._c.keys()
    def values(self):
        return self._c.values()
    def __repr__(self):
        return "Counter(%r)" % self._c

OrderedDict = dict

def _mk_defaultdict():
    class _dd:
        def __init__(self, factory=None, *a, **kw):
            self._factory = factory
            self._d = dict(*a, **kw)
        def __getitem__(self, k):
            if k not in self._d and self._factory is not None:
                self._d[k] = self._factory()
            return self._d[k]
        def __setitem__(self, k, v):
            self._d[k] = v
        def __contains__(self, k):
            return k in self._d
        def __iter__(self):
            return iter(self._d)
        def __len__(self):
            return len(self._d)
        def get(self, k, d=None):
            return self._d.get(k, d)
        def items(self):
            return self._d.items()
        def keys(self):
            return self._d.keys()
        def values(self):
            return self._d.values()
        def __repr__(self):
            return "defaultdict(%r)" % self._d
    return _dd

defaultdict = _mk_defaultdict()

class deque:
    def __init__(self, iterable=None, maxlen=None):
        self._q = list(iterable) if iterable is not None else []
        self.maxlen = maxlen
    def append(self, v):
        self._q.append(v)
        self._trim()
    def appendleft(self, v):
        self._q.insert(0, v)
        self._trim()
    def _trim(self):
        if self.maxlen is not None:
            while len(self._q) > self.maxlen:
                self._q.pop(0)
    def pop(self):
        return self._q.pop()
    def popleft(self):
        return self._q.pop(0)
    def extend(self, it):
        for v in it:
            self.append(v)
    def extendleft(self, it):
        for v in it:
            self.appendleft(v)
    def rotate(self, n=1):
        n = n % len(self._q) if self._q else 0
        self._q = self._q[-n:] + self._q[:-n] if n else self._q
    def clear(self):
        self._q.clear()
    def count(self, v):
        return self._q.count(v)
    def remove(self, v):
        self._q.remove(v)
    def __len__(self):
        return len(self._q)
    def __iter__(self):
        return iter(self._q)
    def __getitem__(self, i):
        return self._q[i]
    def __setitem__(self, i, v):
        self._q[i] = v
    def __repr__(self):
        return "deque(%r)" % self._q
)PY";

const char* src_dataclasses = R"PY(
def _synth_init(fields, defaults):
    # fields: ordered list of (name); defaults: name→value
    code_params = [n for n in fields]
    def __init__(self, *args, **kwargs):
        for idx, n in enumerate(code_params):
            if idx < len(args):
                setattr(self, n, args[idx])
            elif n in kwargs:
                setattr(self, n, kwargs[n])
            elif n in defaults:
                setattr(self, n, defaults[n])
            else:
                raise TypeError("missing required field: " + n)
        for k, v in kwargs.items():
            if k not in code_params:
                setattr(self, k, v)
    return __init__

def _fields_of(cls):
    ann = cls.__dict__.get("__annotations__", {})
    fields = list(ann.keys()) if isinstance(ann, dict) else []
    if not fields:
        # fallback: non-callable non-underscore attrs = fields with defaults
        fields = [n for n in cls.__dict__.keys()
                  if not n.startswith("_")]
    defaults = {n: cls.__dict__[n] for n in fields
                if n in cls.__dict__ and not callable(cls.__dict__[n])}
    return fields, defaults

def dataclass(cls=None, **kw):
    def wrap(c):
        fields, defaults = _fields_of(c)
        c.__init__ = _synth_init(fields, defaults)
        c.__dataclass_fields__ = fields
        return c
    if cls is not None:
        return wrap(cls)
    return wrap

def field(default=None, default_factory=None, **kw):
    if default_factory is not None:
        return default_factory()
    return default

def asdict(obj):
    return {n: getattr(obj, n) for n in obj.__dataclass_fields__}

def astuple(obj):
    return tuple(getattr(obj, n) for n in obj.__dataclass_fields__)

def fields(cls):
    return list(getattr(cls, "__dataclass_fields__", []))
)PY";

const char* src_pathlib = R"PY(
import os.path as _osp

class PurePath:
    def __init__(self, *parts):
        segs = []
        for p in parts:
            if isinstance(p, PurePath):
                segs.append(p._p)
            else:
                segs.append(str(p))
        self._p = _osp.normpath("\\".join(segs)) if segs else "."
    def __truediv__(self, other):
        return PurePath(self._p, str(other))
    def __rtruediv__(self, other):
        return PurePath(str(other), self._p)
    def __str__(self):
        return self._p
    def __repr__(self):
        return "PurePath(%r)" % self._p
    def __eq__(self, other):
        return str(self) == str(other)
    def __hash__(self):
        return hash(self._p)
    @property
    def name(self):
        return _osp.basename(self._p)
    @property
    def stem(self):
        return _osp.splitext(self.name)[0]
    @property
    def suffix(self):
        return _osp.splitext(self.name)[1]
    @property
    def suffixes(self):
        parts = self.name.split(".")
        return ["." + p for p in parts[1:]] if len(parts) > 1 else []
    @property
    def parent(self):
        return PurePath(_osp.dirname(self._p))
    @property
    def parts(self):
        return tuple(s for s in self._p.split("\\") if s)
    def joinpath(self, *parts):
        return PurePath(self._p, *[str(p) for p in parts])
    def with_suffix(self, suf):
        return PurePath(_osp.splitext(self._p)[0] + suf)
    def with_name(self, name):
        return PurePath(_osp.dirname(self._p), name)
    def as_posix(self):
        return self._p.replace("\\", "/")

class Path(PurePath):
    def exists(self):
        return _osp.exists(self._p)
    def is_file(self):
        return _osp.isfile(self._p)
    def is_dir(self):
        return _osp.isdir(self._p)
    def mkdir(self, parents=False, exist_ok=False):
        if parents:
            import os
            os.makedirs(self._p)
        else:
            import os
            os.mkdir(self._p)
    def read_text(self, encoding=None):
        f = open(self._p, "r")
        try:
            return f.read()
        finally:
            f.close()
    def write_text(self, data, encoding=None):
        f = open(self._p, "w")
        try:
            f.write(data)
        finally:
            f.close()
        return len(data)
    def read_bytes(self):
        f = open(self._p, "rb")
        try:
            return f.read()
        finally:
            f.close()
    def write_bytes(self, data):
        f = open(self._p, "wb")
        try:
            f.write(data)
        finally:
            f.close()
        return len(data)
    def iterdir(self):
        import os
        return [Path(self._p, n) for n in os.listdir(self._p)]
    def glob(self, pattern):
        import os
        import re as _re
        pat = pattern.replace("*", ".*").replace("?", ".")
        rx = _re.compile("^" + pat + "$")
        out = []
        for n in os.listdir(self._p):
            if rx.match(n):
                out.append(Path(self._p, n))
        return out

PureWindowsPath = PurePath
WindowsPath = Path
)PY";

PyRef mod_datetime(interpreter& i) { return boot(i, "datetime", src_datetime); }
PyRef mod_enum(interpreter& i) { return boot(i, "enum", src_enum); }
PyRef mod_collections(interpreter& i) {
    return boot(i, "collections", src_collections);
}
PyRef mod_dataclasses(interpreter& i) {
    return boot(i, "dataclasses", src_dataclasses);
}
PyRef mod_pathlib(interpreter& i) {
    return boot(i, "pathlib", src_pathlib);
}

} // namespace

// ── pymini-scope externs used by other TUs ────────────────────────────────
PyRef pymini_open_file(interpreter& i, const std::string& path_utf8,
                       const std::string& mode) {
    return open_path(i, path_utf8, mode);
}

PyRef file_lines(const PyRef& file_obj) {
    auto* f = as_file(file_obj);
    if (!f || !f->pimpl || !f->pimpl->fp)
        return py_list();
    std::vector<PyRef> lines;
    std::string cur;
    int c;
    while ((c = std::fgetc(f->pimpl->fp)) != EOF) {
        cur += static_cast<char>(c);
        if (c == '\n') {
            lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
            cur.clear();
        }
    }
    if (!cur.empty())
        lines.push_back(f->pimpl->binary ? py_bytes(cur) : py_str(cur));
    return py_list(std::move(lines));
}

// ── extended builtin members for stdlib2 kinds ────────────────────────────
PyRef pymini_extra_member(interpreter& i, const PyRef& obj,
                          const std::string& name) {
    if (!obj)
        return nullptr;
    switch (obj->kind) {
    case py_kind::file_: {
        using namespace file_impl;
        if (name == "read")      return member_fn(name, obj, m_read);
        if (name == "readline")  return member_fn(name, obj, m_readline);
        if (name == "readlines") return member_fn(name, obj, m_readlines);
        if (name == "write")     return member_fn(name, obj, m_write);
        if (name == "writelines") return member_fn(name, obj, m_writelines);
        if (name == "flush")     return member_fn(name, obj, m_flush);
        if (name == "seek")      return member_fn(name, obj, m_seek);
        if (name == "tell")      return member_fn(name, obj, m_tell);
        if (name == "close")     return member_fn(name, obj, m_close);
        if (name == "__enter__") return member_fn(name, obj, m_enter);
        if (name == "__exit__")  return member_fn(name, obj, m_exit);
        if (name == "seekable")  return member_fn(name, obj, m_seekable);
        if (name == "readable")  return member_fn(name, obj, m_readable);
        if (name == "writable")  return member_fn(name, obj, m_writable);
        if (name == "closed") {
            auto* f = as_file(obj);
            return py_bool(!f->pimpl || !f->pimpl->fp);
        }
        if (name == "mode")
            return py_str(as_file(obj)->mode);
        return nullptr;
    }
    case py_kind::thread_: {
        using namespace thread_impl;
        if (name == "start")      return member_fn(name, obj, m_start);
        if (name == "join")       return member_fn(name, obj, m_join);
        if (name == "is_alive")   return member_fn(name, obj, m_is_alive);
        if (name == "name")       return py_str(as_thread(obj)->pimpl->name);
        return nullptr;
    }
    case py_kind::lock_: {
        using namespace thread_impl;
        if (name == "acquire")   return member_fn(name, obj, m_lock_acquire);
        if (name == "release")   return member_fn(name, obj, m_lock_release);
        if (name == "locked")    return member_fn(name, obj, m_lock_locked);
        if (name == "__enter__") return member_fn(name, obj, m_lock_enter);
        if (name == "__exit__")  return member_fn(name, obj, m_lock_exit);
        return nullptr;
    }
    case py_kind::event_: {
        using namespace thread_impl;
        if (name == "set")     return member_fn(name, obj, m_event_set);
        if (name == "clear")   return member_fn(name, obj, m_event_clear);
        if (name == "is_set")  return member_fn(name, obj, m_event_is_set);
        if (name == "wait")    return member_fn(name, obj, m_event_wait);
        return nullptr;
    }
    case py_kind::queue_: {
        using namespace queue_impl;
        if (name == "put")         return member_fn(name, obj, m_put);
        if (name == "get")         return member_fn(name, obj, m_get);
        if (name == "put_nowait")  return member_fn(name, obj, m_put_nowait);
        if (name == "get_nowait")  return member_fn(name, obj, m_get_nowait);
        if (name == "qsize")       return member_fn(name, obj, m_qsize);
        if (name == "empty")       return member_fn(name, obj, m_empty);
        if (name == "full")        return member_fn(name, obj, m_full);
        if (name == "task_done")   return member_fn(name, obj, m_task_done);
        if (name == "join")        return member_fn(name, obj, m_q_join);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// ── registration ─────────────────────────────────────────────────────────
void pymini_register_stdlib_factories_2(interpreter& i) {
    i.stdlib_factories["os"] = mod_os;
    i.stdlib_factories["os.path"] = mod_ospath;
    i.stdlib_factories["sys"] = mod_sys;
    i.stdlib_factories["io"] = mod_io;
    i.stdlib_factories["threading"] = mod_threading;
    i.stdlib_factories["queue"] = mod_queue;
    i.stdlib_factories["logging"] = mod_logging;
    i.stdlib_factories["copy"] = mod_copy;
    i.stdlib_factories["traceback"] = mod_traceback;
    i.stdlib_factories["warnings"] = mod_warnings;
    i.stdlib_factories["abc"] = mod_abc;
    i.stdlib_factories["typing"] = mod_typing;
    i.stdlib_factories["contextlib"] = mod_contextlib;
    i.stdlib_factories["bisect"] = mod_bisect;
    i.stdlib_factories["heapq"] = mod_heapq;
    i.stdlib_factories["itertools"] = mod_itertools;
    i.stdlib_factories["functools"] = mod_functools;
    i.stdlib_factories["collections"] = mod_collections;
    i.stdlib_factories["dataclasses"] = mod_dataclasses;
    i.stdlib_factories["datetime"] = mod_datetime;
    i.stdlib_factories["enum"] = mod_enum;
    i.stdlib_factories["pathlib"] = mod_pathlib;
}

} // namespace sao::plugins::pymini
