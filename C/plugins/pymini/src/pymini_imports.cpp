// Preflight and runtime share package precedence and search paths.
#include "pymini_interp.h"
#include "pymini_parser.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>

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

constexpr std::size_t kMaxFileBytes = 1024 * 1024;
constexpr std::size_t kMaxTotalBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxFiles = 256;
constexpr std::size_t kMaxDepth = 64;

fs::path utf8_path(const std::string& text) {
    return fs::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

[[noreturn]] void import_error(const std::string& kind, const std::string& message,
                               const std::string& file, src_pos pos = {1, 1}) {
    py_error e{};
    e.kind = kind;
    e.message = message;
    e.file = file;
    e.pos = pos;
    throw e;
}

std::string read_source(const fs::path& file) {
    const auto label = narrow(file.wstring());
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) import_error("OSError", "cannot open module", label);
    const auto size = in.tellg();
    if (size < 0) import_error("OSError", "cannot size module", label);
    if (size > static_cast<std::streamoff>(kMaxFileBytes))
        import_error("MemoryError", "preflight file byte budget exceeded", label);
    std::string source(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(source.data(), static_cast<std::streamsize>(source.size())))
        import_error("OSError", "cannot read complete module", label);
    if (in.peek() != std::char_traits<char>::eof())
        import_error("OSError", "module changed while reading", label);
    return source;
}

ast_module parse_checked(std::string_view source, const std::string& file) {
    if (source.size() > kMaxFileBytes)
        import_error("MemoryError", "preflight file byte budget exceeded", file);
    const auto tokens = lex_source(source, file);
    if (tokens.size() > 131072)
        import_error("MemoryError", "preflight token budget exceeded", file);
    std::vector<tok_kind> brackets;
    std::size_t indent = 0;
    std::size_t expression_tokens = 0;
    for (const auto& t : tokens) {
        switch (t.kind) {
        case tok_kind::lparen: case tok_kind::lbracket: case tok_kind::lbrace:
            brackets.push_back(t.kind);
            break;
        case tok_kind::rparen: case tok_kind::rbracket: case tok_kind::rbrace: {
            const auto expected = t.kind == tok_kind::rparen ? tok_kind::lparen
                : t.kind == tok_kind::rbracket ? tok_kind::lbracket : tok_kind::lbrace;
            if (brackets.empty() || brackets.back() != expected)
                import_error("SyntaxError", "unmatched closing delimiter", file, t.pos);
            brackets.pop_back();
            break;
        }
        case tok_kind::indent: ++indent; break;
        case tok_kind::dedent: if (indent) --indent; break;
        default: break;
        }
        if (t.kind == tok_kind::newline || t.kind == tok_kind::semicolon)
            expression_tokens = 0;
        else
            ++expression_tokens;
        if (brackets.size() + indent > kMaxDepth || expression_tokens > 512)
            import_error("RecursionError", "preflight syntax budget exceeded", file, t.pos);
    }
    if (!brackets.empty()) import_error("SyntaxError", "unclosed delimiter", file);
    return parse_source(source, file);
}

std::string parent_name(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? std::string{} : name.substr(0, dot);
}

std::string relative_name(const std::string& package, const std::string& name,
                           int level, const std::string& file, src_pos pos) {
    if (level == 0) return name;
    if (level < 0 || package.empty())
        import_error("ImportError", "relative import has no known parent package", file, pos);
    std::string base = package;
    for (int n = 1; n < level; ++n) {
        const auto dot = base.rfind('.');
        if (dot == std::string::npos)
            import_error("ImportError", "relative import beyond top-level package", file, pos);
        base.resize(dot);
    }
    return name.empty() ? base : base + "." + name;
}

struct module_spec {
    fs::path file;
    std::vector<fs::path> paths;
    bool native = false;
    bool package = false;
    bool found = false;
};

bool native_name(const interpreter& i, const std::string& name) {
    return i.stdlib_factories.count(name) || name == "builtins" ||
           name == "__future__" || name == "act_platform" || name == "sao_sdk" ||
           name == "act_platform.plugins" || name == "act_platform.ui" ||
           name == "sao_sdk.ui";
}

module_spec resolve_child(const interpreter& i, const std::vector<fs::path>& dirs,
                           const std::string& name) {
    module_spec spec;
    if (native_name(i, name)) {
        spec.native = spec.found = true;
        const std::string prefix = name + ".";
        spec.package = name == "act_platform" || name == "sao_sdk";
        for (const auto& item : i.stdlib_factories)
            if (item.first.compare(0, prefix.size(), prefix) == 0)
                spec.package = true;
        return spec;
    }
    const auto dot = name.rfind('.');
    const fs::path leaf = utf8_path(name.substr(dot == std::string::npos ? 0 : dot + 1));
    for (const auto& dir : dirs) {
        const fs::path package = dir / leaf;
        const fs::path init = package / L"__init__.py";
        const fs::path file = dir / fs::path(leaf.wstring() + L".py");
        if (fs::is_regular_file(init)) return {init, {package}, false, true, true};
        if (fs::is_regular_file(file)) return {file, {}, false, false, true};
        if (fs::is_directory(package)) spec.paths.push_back(package);
    }
    spec.found = spec.package = !spec.paths.empty();
    return spec;
}

std::vector<fs::path> search_dirs(const interpreter& i) {
    std::vector<fs::path> dirs;
    for (const auto& path : i.cfg.module_dirs) dirs.emplace_back(path);
    if (dirs.empty() && !i.cfg.plugin_root.empty()) dirs.emplace_back(i.cfg.plugin_root);
    return dirs;
}

PyDictObj* module_dict(const PyRef& module) {
    if (auto* d = as_dict(module)) return d;
    if (auto* m = as_module(module)) return as_dict(m->dict);
    if (module && module->kind == py_kind::module_proxy)
        return as_dict(static_cast<PyModuleProxyObj*>(module.get())->dict);
    return nullptr;
}

void package_metadata(const PyRef& module, const module_spec& spec) {
    if (!spec.package) return;
    std::vector<PyRef> paths;
    for (const auto& p : spec.paths) paths.push_back(py_str(narrow(p.wstring())));
    dict_set(module_dict(module), py_str("__path__"), py_list(std::move(paths)));
}

bool catches_import(const ast_expr* expr) {
    if (!expr) return true;
    if (expr->tag == et::name)
        return expr->name == "ImportError" || expr->name == "ModuleNotFoundError";
    if (expr->tag == et::tuple_lit)
        for (const auto& part : expr->parts)
            if (catches_import(part.get())) return true;
    return false;
}

struct assessment {
    interpreter resolver{interpreter::config{}};
    std::unordered_map<std::string, module_spec> modules;
    std::unordered_set<std::wstring> files;
    std::size_t total_bytes = 0;
    std::string reason;
    bool subset_only = false;

    static void target_names(const ast_expr* e, std::unordered_map<std::string, std::size_t>& writes) {
        if (!e) return;
        if (e->tag == et::name) ++writes[e->name];
        if (e->tag == et::tuple_lit || e->tag == et::list_lit)
            for (const auto& part : e->parts) target_names(part.get(), writes);
        if (e->tag == et::star_) target_names(e->base.get(), writes);
    }

    static void expression_bindings(
        const ast_expr* e,
        std::unordered_map<std::string, std::size_t>& writes) {
        if (!e)
            return;
        if (e->tag == et::lambda_) {
            for (const auto& param : e->params)
                expression_bindings(param.default_value.get(), writes);
            return;
        }
        if (e->tag == et::named_expr)
            ++writes[e->name];
        expression_bindings(e->base.get(), writes);
        expression_bindings(e->index.get(), writes);
        expression_bindings(e->orelse.get(), writes);
        expression_bindings(e->step.get(), writes);
        for (const auto& part : e->parts)
            expression_bindings(part.get(), writes);
        for (const auto& arg : e->call_args)
            expression_bindings(arg.value.get(), writes);
        for (const auto& clause : e->generators) {
            expression_bindings(clause.iter.get(), writes);
            for (const auto& cond : clause.ifs)
                expression_bindings(cond.get(), writes);
        }
    }

    static void binding_counts(const std::vector<stmt_ptr>& body,
                               std::unordered_map<std::string, std::size_t>& writes,
                               const std::string& file, std::size_t depth = 0) {
        if (body.empty()) return;
        if (depth > kMaxDepth)
            import_error("RecursionError", "preflight binding depth exceeded", file);
        for (const auto& stmt : body) {
            const auto& s = *stmt;
            if (s.tag == st::funcdef || s.tag == st::classdef) ++writes[s.name];
            expression_bindings(s.value.get(), writes);
            expression_bindings(s.value2.get(), writes);
            for (const auto& t : s.targets) target_names(t.get(), writes);
            for (const auto& item : s.with_items) {
                expression_bindings(item.ctx_expr.get(), writes);
                target_names(item.target.get(), writes);
            }
            for (const auto& decorator : s.decorators)
                expression_bindings(decorator.get(), writes);
            for (const auto& base : s.bases)
                expression_bindings(base.get(), writes);
            for (const auto& [name, base] : s.kw_bases) {
                (void)name;
                expression_bindings(base.get(), writes);
            }
            for (const auto& param : s.params)
                expression_bindings(param.default_value.get(), writes);
            for (const auto& item : s.imports) {
                const auto name = !item.second.empty() ? item.second
                    : s.tag == st::import ? item.first.substr(0, item.first.find('.')) : item.first;
                ++writes[name];
            }
            if (s.tag == st::funcdef || s.tag == st::classdef)
                continue;
            binding_counts(s.body, writes, file, depth + 1);
            binding_counts(s.orelse, writes, file, depth + 1);
            binding_counts(s.final, writes, file, depth + 1);
            for (const auto& arm : s.except_arms) {
                if (!arm.name.empty()) ++writes[arm.name];
                expression_bindings(arm.type.get(), writes);
                binding_counts(arm.body, writes, file, depth + 1);
            }
        }
    }

    assessment() {
        // Only factory pointers are registered; assessment never calls them.
        pymini_register_stdlib_factories_1(resolver);
        pymini_register_stdlib_factories_2(resolver);
    }

    void unsupported(const std::string& file, src_pos pos, const std::string& why) {
        if (reason.empty()) reason = file + ":" + std::to_string(pos.line) + ": " + why;
    }

    void source(std::string_view text, const std::string& file,
                const std::string& package, std::size_t depth) {
        ast_module parsed;
        try {
            parsed = parse_checked(text, file);
        } catch (const py_error& e) {
            if (e.kind != "UnsupportedSyntax") throw;
            unsupported(e.file, e.pos, e.message);
            return;
        }
        std::unordered_map<std::string, std::size_t> writes;
        binding_counts(parsed.body, writes, file);
        std::unordered_set<std::string> type_only;
        walk(parsed.body, file, package, depth, false, &writes, &type_only);
    }

    void visit(const std::string& name, const module_spec& spec, std::size_t depth) {
        if (spec.native || spec.file.empty()) return;
        std::wstring key = fs::canonical(spec.file).wstring();
        std::transform(key.begin(), key.end(), key.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        key += L"|" + utf8_path(name).wstring();
        if (files.count(key)) return;
        const std::string file = narrow(spec.file.wstring());
        if (depth > kMaxDepth || files.size() >= kMaxFiles)
            import_error("RecursionError", "preflight import graph budget exceeded", file);
        files.insert(std::move(key));
        const auto text = read_source(spec.file);
        if (text.size() > kMaxTotalBytes - total_bytes)
            import_error("MemoryError", "preflight total byte budget exceeded", file);
        total_bytes += text.size();
        source(text, file, spec.package ? name : parent_name(name), depth);
    }

    bool load(const std::string& name, const std::string& file, src_pos pos,
              std::size_t depth, bool optional, bool attribute = false) {
        if (name.empty()) import_error("ImportError", "empty module name", file, pos);
        if (subset_only) {
            const auto root = name.substr(0, name.find('.'));
            if (!native_name(resolver, root) &&
                interpreter::cpython_only_roots().count(root) && !optional)
                unsupported(file, pos, "imports external CPython module '" + root + "'");
            return true;
        }
        auto dirs = search_dirs(resolver);
        std::size_t start = 0;
        std::size_t components = 0;
        while (start < name.size()) {
            if (++components > kMaxDepth)
                import_error("RecursionError", "preflight module name depth exceeded", file, pos);
            const auto dot = name.find('.', start);
            const std::string logical = name.substr(0, dot);
            module_spec spec;
            if (const auto it = modules.find(logical); it != modules.end()) spec = it->second;
            else spec = resolve_child(resolver, dirs, logical);
            if (!spec.found) {
                if (optional || (attribute && dot == std::string::npos)) return false;
                if (start == 0 && interpreter::cpython_only_roots().count(logical)) {
                    unsupported(file, pos, "imports external CPython module '" + logical + "'");
                    return false;
                }
                import_error("ModuleNotFoundError", "No module named '" + logical + "'", file, pos);
            }
            if (modules.size() >= 1024 && !modules.count(logical))
                import_error("MemoryError", "preflight module budget exceeded", file, pos);
            modules[logical] = spec;
            visit(logical, spec, depth + 1);
            if (dot == std::string::npos) return true;
            if (!spec.package) {
                if (optional || attribute) return false;
                import_error("ModuleNotFoundError", "'" + logical + "' is not a package", file, pos);
            }
            dirs = spec.paths;
            start = dot + 1;
        }
        import_error("ImportError", "invalid module name '" + name + "'", file, pos);
    }

    void walk(const std::vector<stmt_ptr>& body, const std::string& file,
              const std::string& package, std::size_t depth, bool optional,
              const std::unordered_map<std::string, std::size_t>* writes = nullptr,
              std::unordered_set<std::string>* type_only = nullptr) {
        if (depth > kMaxDepth)
            import_error("RecursionError", "preflight traversal depth exceeded", file);
        for (const auto& stmt : body) {
            const auto& s = *stmt;
            if (s.tag == st::import) {
                for (const auto& item : s.imports) load(item.first, file, s.pos, depth, optional);
            } else if (s.tag == st::import_from) {
                if (subset_only && s.from_level) continue;
                const auto base = relative_name(package, s.from_module, s.from_level, file, s.pos);
                if (load(base, file, s.pos, depth, optional) && !subset_only) {
                    const auto spec = modules.find(base);
                    if (spec != modules.end() && spec->second.package)
                        for (const auto& item : s.imports)
                            if (item.first != "*")
                                load(base + "." + item.first, file, s.pos, depth, optional, true);
                }
            }
            if (writes && type_only && s.tag == st::import_from &&
                s.from_level == 0 && s.from_module == "typing" &&
                (subset_only || (modules.count("typing") && modules.at("typing").native))) {
                for (const auto& item : s.imports) {
                    const auto& binding = item.second.empty() ? item.first : item.second;
                    const auto count = writes->find(binding);
                    if (item.first == "TYPE_CHECKING" && !writes->count("*") &&
                        count != writes->end() && count->second == 1)
                        type_only->insert(binding);
                }
            }
            if (type_only && s.tag == st::if_ && s.value && s.value->tag == et::name &&
                type_only->count(s.value->name)) {
                walk(s.orelse, file, package, depth + 1, optional);
                continue;
            }
            bool guarded = optional;
            if (s.tag == st::try_)
                for (const auto& arm : s.except_arms)
                    guarded = guarded || catches_import(arm.type.get());
            if (s.tag == st::funcdef) guarded = false;
            if (!s.body.empty()) {
                const bool straight_line = s.tag == st::suite;
                walk(s.body, file, package, depth + 1, guarded,
                    straight_line ? writes : nullptr, straight_line ? type_only : nullptr);
            }
            if (!s.orelse.empty()) walk(s.orelse, file, package, depth + 1, optional);
            if (!s.final.empty()) walk(s.final, file, package, depth + 1, optional);
            for (const auto& arm : s.except_arms)
                walk(arm.body, file, package, depth + 1, optional);
        }
    }
};

} // namespace

std::string pymini_configure_imports(interpreter::config& cfg,
                                     const std::wstring& plugin_root,
                                     const std::wstring& entry_rel,
                                     const std::vector<std::wstring>& extra_dirs) {
    if (extra_dirs.size() > 128)
        import_error("MemoryError", "preflight search directory budget exceeded", narrow(entry_rel));
    fs::path root = fs::absolute(plugin_root).lexically_normal();
    if (root.filename().empty() && root.has_relative_path()) root = root.parent_path();
    const fs::path entry = fs::path(entry_rel).lexically_normal();
    if (entry.empty() || entry.is_absolute() || entry.has_root_name() || entry.extension() != L".py")
        import_error("ImportError", "entry must be a relative .py path", narrow(entry_rel));
    for (const auto& part : entry)
        if (part == L"..") import_error("ImportError", "entry escapes plugin root", narrow(entry_rel));
    cfg.plugin_root = root.wstring();
    cfg.module_dirs.clear();
    auto add = [&](const fs::path& dir) {
        const auto text = fs::absolute(dir).lexically_normal().wstring();
        if (std::find(cfg.module_dirs.begin(), cfg.module_dirs.end(), text) == cfg.module_dirs.end())
            cfg.module_dirs.push_back(text);
    };
    add(root);
    const wchar_t* subdirs[] = {L"vendor", L"vendors", L"libs", L"lib", L"site-packages", L"python"};
    for (const auto* sub : subdirs) add(root / sub);
    for (const auto& dir : extra_dirs) add(dir);
    fs::path logical = entry;
    logical.replace_extension();
    if (logical.filename() == L"__init__") logical = logical.parent_path();
    if (logical.empty()) {
        logical = root.filename();
        add(root.parent_path());
    }
    std::string name = narrow(logical.generic_wstring());
    std::replace(name.begin(), name.end(), '/', '.');
    if (name.empty()) import_error("ImportError", "entry has no module name", narrow(entry_rel));
    return name;
}

bool pymini_preflight_plugin(const std::wstring& plugin_root,
                              const std::wstring& entry_rel,
                              const std::vector<std::wstring>& extra_dirs,
                              std::string* out_reason) {
    if (out_reason) out_reason->clear();
    try {
        assessment scan;
        const auto name = pymini_configure_imports(scan.resolver.cfg, plugin_root, entry_rel, extra_dirs);
        module_spec entry;
        entry.file = fs::path(scan.resolver.cfg.plugin_root) / entry_rel;
        entry.found = true;
        entry.package = entry.file.filename() == L"__init__.py";
        if (entry.package) entry.paths.push_back(entry.file.parent_path());
        scan.modules[name] = entry;
        const auto parent = parent_name(name);
        if (!parent.empty()) scan.load(parent, narrow(entry.file.wstring()), {1, 1}, 0, false);
        scan.visit(name, entry, 0);
        if (out_reason) *out_reason = scan.reason;
        return scan.reason.empty();
    } catch (const fs::filesystem_error& e) {
        import_error("OSError", e.what(), narrow((fs::path(plugin_root) / entry_rel).wstring()));
    }
}

bool pymini_preflight_subset(std::string_view source, std::string* out_reason) {
    if (out_reason) out_reason->clear();
    assessment scan;
    scan.subset_only = true;
    scan.source(source, "<source>", {}, 0);
    if (out_reason) *out_reason = scan.reason;
    return scan.reason.empty();
}

PyRef interpreter::import_dotted(const std::string& dotted_in, frame* from, int level) {
    gil_guard guard(*this);
    std::string package;
    if (from)
        if (auto* text = as_str(dict_get(as_dict(from->globals), py_str("__package__"))))
            package = text->v;
    std::string dotted;
    try {
        dotted = relative_name(package, dotted_in, level,
            from ? from->file : std::string{}, {from ? from->line : 1, 1});
    } catch (const py_error& e) {
        raise_exc(e.kind, e.message, e.pos);
    }
    if (dotted.empty()) raise_exc("ImportError", "cannot resolve empty import", {});
    if (PyRef cached = find_loaded(dotted)) return cached;
    auto dirs = search_dirs(*this);
    PyRef parent;
    std::size_t start = 0;
    std::size_t components = 0;
    try {
        while (start < dotted.size()) {
            if (++components > kMaxDepth || pending.size() >= kMaxDepth)
                raise_exc("RecursionError", "module import depth exceeded", {});
            const auto dot = dotted.find('.', start);
            const auto logical = dotted.substr(0, dot);
            const auto leaf = dotted.substr(start, dot == std::string::npos ? dot : dot - start);
            PyRef module = find_loaded(logical);
            if (!module && parent && native_name(*this, logical)) {
                module = getattr(parent, leaf);
                if (module) {
                    if (auto* m = as_module(module)) {
                        const auto spec = resolve_child(*this, {}, logical);
                        m->package = spec.package ? logical : parent_name(logical);
                        dict_set(module_dict(module), py_str("__package__"), py_str(m->package));
                        package_metadata(module, spec);
                    }
                    dict_set(as_dict(sys_modules), py_str(logical), module);
                }
            }
            if (!module) {
                const auto spec = resolve_child(*this, dirs, logical);
                if (!spec.found)
                    raise_exc("ModuleNotFoundError", "No module named '" + logical + "'", {});
                if (spec.native) {
                    if (logical == "builtins") {
                        module = create_module_object(logical);
                        as_module(module)->dict = builtins_dict;
                    } else if (logical == "__future__") {
                        module = create_module_object(logical);
                        dict_set(module_dict(module), py_str("annotations"), py_none());
                    } else {
                        const auto factory = stdlib_factories.find(logical);
                        if (factory == stdlib_factories.end())
                            raise_exc("ModuleNotFoundError", "host module unavailable: '" + logical + "'", {});
                        module = factory->second(*this);
                    }
                    if (logical == "typing")
                        dict_set(module_dict(module), py_str("TYPE_CHECKING"), py_false());
                    if (auto* m = as_module(module)) {
                        m->package = spec.package ? logical : parent_name(logical);
                        dict_set(module_dict(module), py_str("__package__"), py_str(m->package));
                    }
                    package_metadata(module, spec);
                    dict_set(as_dict(sys_modules), py_str(logical), module);
                } else {
                    module = create_module_object(logical, narrow(spec.file.wstring()),
                        spec.package ? logical : parent_name(logical));
                    package_metadata(module, spec);
                    if (spec.file.empty()) {
                        register_module(module);
                    } else {
                        const auto text = read_source(spec.file);
                        exec_module_source(logical, narrow(spec.file.wstring()), text, module);
                    }
                }
            }
            if (parent)
                if (auto* d = module_dict(parent)) dict_set(d, py_str(leaf), module);
            if (dot == std::string::npos) return module;
            PyRef path = getattr(module, "__path__");
            const bool native_parent = native_name(*this, logical);
            if (!path && !native_parent)
                raise_exc("ModuleNotFoundError", "'" + logical + "' is not a package", {});
            dirs.clear();
            if (auto* list = as_list(path))
                for (const auto& item : list->v)
                    if (const auto* text = as_str(item)) dirs.push_back(utf8_path(text->v));
            parent = module;
            start = dot + 1;
        }
    } catch (const fs::filesystem_error& e) {
        raise_exc("OSError", e.what(), {});
    }
    raise_exc("ImportError", "invalid module name '" + dotted + "'", {});
}

PyRef interpreter::import_from(PyRef module, const std::string& name) {
    PyRef value = dict_get(module_dict(module), py_str(name));
    if (!value) value = getattr(module, name);
    if (value) {
        if (auto* child = as_module(value))
            if (native_name(*this, child->name) && !find_loaded(child->name)) {
                const auto spec = resolve_child(*this, {}, child->name);
                child->package = spec.package ? child->name : parent_name(child->name);
                dict_set(module_dict(value), py_str("__package__"), py_str(child->package));
                package_metadata(value, spec);
                dict_set(as_dict(sys_modules), py_str(child->name), value);
            }
        return value;
    }
    if (getattr(module, "__path__")) {
        if (auto* m = as_module(module))
            return import_dotted(m->name + "." + name, nullptr);
    }
    raise_exc("ImportError", "cannot import name '" + name + "' from module", {});
}

const std::unordered_set<std::string>& interpreter::cpython_only_roots() {
    static const std::unordered_set<std::string> roots = {
        // C-extension / eval-only roots that pymini deliberately cannot
        // emulate.  Kept in sync with docs/legacy-compat-design.md §subset.
        "ctypes", "cffi", "_ctypes", "numpy", "scipy", "pandas", "PIL",
        "cv2", "pygame", "wx", "PyQt5", "PyQt6", "PySide2", "PySide6",
        "tkinter", "multiprocessing", "subprocess", "signal", "socket",
        "ssl", "sqlite3", "_socket", "_ssl", "_sqlite3", "select",
        "selectors", "asyncio", "concurrent", "inspect", "dis", "marshal",
        "gc", "weakref", "_imp", "site",
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

} // namespace sao::plugins::pymini
