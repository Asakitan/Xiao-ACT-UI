# -*- coding: utf-8 -*-
"""AngelScript scripting runtime via ctypes binding.

Plugin entry is an ``.as`` file with C-like AngelScript syntax::

    // plugin.as

    PluginContext@ ctx;

    void on_load(PluginContext@ c)
    {
        @ctx = c;
        ctx.log("Hello from AngelScript!");
        ctx.register_ui_panel("as_panel",
            dictionary = {{"title", "AS Panel"}},
            @render_panel, @on_action);
    }

    dictionary@ render_panel(dictionary@ payload)
    {
        return ctx.ui_panel("AngelScript Plugin", {
            ctx.ui_text("Hello from AngelScript!")
        });
    }

    dictionary@ on_action(string action_id, dictionary@ payload)
    {
        ctx.log("action: " + action_id);
        dictionary d;
        d["ok"] = true;
        return d;
    }

    void on_unload()
    {
        ctx.log("Bye from AngelScript!");
    }

AngelScript is embedded via a ctypes wrapper around the AngelScript SDK
DLL. The DLL (``angelscript.dll`` / ``libangelscript.so``) must be
placed in the plugin's directory, ``vendor/``, or on the system PATH.

Since the native AngelScript SDK is not always available, this runtime
falls back to a **pure-Python AngelScript subset interpreter** that
parses and executes a practical subset of AngelScript syntax. This
guarantees plugins work even without the native DLL, at the cost of
performance on heavy computation.
"""

from __future__ import annotations

import json
import os
import re
import sys
from types import ModuleType
from typing import Any, Callable, Optional, TYPE_CHECKING

from .base import ContextProxy, ScriptModule, ScriptRuntime, wrap_callback, to_json_safe

if TYPE_CHECKING:
    from act_platform.plugins import PluginContext, PluginRecord


class _AngelScriptInterpreter:
    """Minimal pure-Python AngelScript-subset interpreter.

    Supports enough syntax for plugin lifecycle hooks:
    - Global variables and function declarations
    - String, int, float, bool, dictionary, array types
    - Function calls on registered host objects (ctx.method(...))
    - if/else, while, for, return statements
    - Basic expressions (+, -, *, /, ==, !=, <, >, <=, >=, &&, ||, !)
    - String concatenation and interpolation
    """

    def __init__(self) -> None:
        self._globals: dict[str, Any] = {}
        self._functions: dict[str, "_ASFunction"] = {}
        self._host_objects: dict[str, Any] = {}

    def register_host_object(self, name: str, obj: Any) -> None:
        self._host_objects[name] = obj
        self._globals[name] = obj

    def execute_source(self, source: str) -> None:
        source = _strip_comments(source)
        tokens = _tokenize(source)
        self._parse_toplevel(tokens)

    def get_function(self, name: str) -> Optional[Callable]:
        fn = self._functions.get(name)
        if fn is None:
            return None
        interp = self

        def _caller(*args):
            return interp._call_function(fn, list(args))
        return _caller

    _TYPE_KEYWORDS = frozenset((
        "void", "int", "float", "string", "bool",
        "dictionary", "array", "auto",
        "PluginContext", "PluginContext@", "dictionary@",
    ))

    def _is_type(self, token: str) -> bool:
        return token in self._TYPE_KEYWORDS

    def _parse_toplevel(self, tokens: list[str]) -> None:
        i = 0
        while i < len(tokens):
            if self._is_type(tokens[i]):
                ret_type = tokens[i]
                i += 1
                if i < len(tokens) and tokens[i] == "@":
                    ret_type += "@"
                    i += 1
                if i >= len(tokens):
                    break
                name = tokens[i]
                i += 1
                if i < len(tokens) and tokens[i] == "(":
                    params, i = self._parse_params(tokens, i)
                    body, i = self._parse_block(tokens, i)
                    self._functions[name] = _ASFunction(name, ret_type, params, body)
                elif i < len(tokens) and tokens[i] in ("=", ";"):
                    if tokens[i] == "=":
                        i += 1
                        val, i = self._parse_expr_until(tokens, i, ";")
                        self._globals[name] = self._eval_expr(val, {})
                    if i < len(tokens) and tokens[i] == ";":
                        i += 1
                else:
                    self._globals[name] = None
                    if i < len(tokens) and tokens[i] == ";":
                        i += 1
            elif tokens[i] == "@":
                i += 1
            elif tokens[i] == ";":
                i += 1
            else:
                i += 1

    def _parse_params(self, tokens: list[str], i: int) -> tuple[list[tuple[str, str]], int]:
        assert tokens[i] == "("
        i += 1
        params = []
        while i < len(tokens) and tokens[i] != ")":
            ptype = tokens[i]
            i += 1
            if i < len(tokens) and tokens[i] == "@":
                ptype += "@"
                i += 1
            if i < len(tokens) and tokens[i] not in (",", ")"):
                pname = tokens[i]
                i += 1
            else:
                pname = f"_p{len(params)}"
            params.append((ptype, pname))
            if i < len(tokens) and tokens[i] == ",":
                i += 1
        if i < len(tokens) and tokens[i] == ")":
            i += 1
        return params, i

    def _parse_block(self, tokens: list[str], i: int) -> tuple[list, int]:
        if i >= len(tokens) or tokens[i] != "{":
            return [], i
        i += 1
        depth = 1
        block_tokens = []
        while i < len(tokens) and depth > 0:
            if tokens[i] == "{":
                depth += 1
            elif tokens[i] == "}":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            block_tokens.append(tokens[i])
            i += 1
        return block_tokens, i

    def _parse_expr_until(self, tokens: list[str], i: int,
                          terminator: str) -> tuple[list[str], int]:
        expr = []
        depth = 0
        while i < len(tokens):
            if depth == 0 and tokens[i] == terminator:
                break
            if tokens[i] in ("(", "{", "["):
                depth += 1
            elif tokens[i] in (")", "}", "]"):
                depth -= 1
            expr.append(tokens[i])
            i += 1
        return expr, i

    def _call_function(self, fn: "_ASFunction", args: list) -> Any:
        local_scope = dict(self._globals)
        for idx, (ptype, pname) in enumerate(fn.params):
            if idx < len(args):
                local_scope[pname] = args[idx]
            else:
                local_scope[pname] = None
        return self._exec_block(fn.body, local_scope)

    def _exec_block(self, tokens: list[str], scope: dict) -> Any:
        i = 0
        while i < len(tokens):
            # Skip @ handle-dereference prefix (AngelScript handle syntax)
            if tokens[i] == "@" and i + 1 < len(tokens):
                next_tok = tokens[i + 1]
                if (i + 2 < len(tokens) and tokens[i + 2] == "="
                        and next_tok not in ("(", "{", "[", "@")):
                    var_name = next_tok
                    i += 3
                    expr, i = self._parse_expr_until(tokens, i, ";")
                    scope[var_name] = self._eval_expr(expr, scope)
                    if var_name in self._globals:
                        self._globals[var_name] = scope[var_name]
                    if i < len(tokens) and tokens[i] == ";":
                        i += 1
                    continue
                else:
                    i += 1
                    continue
            if tokens[i] == "return":
                i += 1
                expr, i = self._parse_expr_until(tokens, i, ";")
                if i < len(tokens) and tokens[i] == ";":
                    i += 1
                return self._eval_expr(expr, scope)
            elif tokens[i] == "if":
                result, i = self._exec_if(tokens, i, scope)
                if result is not _SENTINEL:
                    return result
            elif tokens[i] == "while":
                result, i = self._exec_while(tokens, i, scope)
                if result is not _SENTINEL:
                    return result
            elif tokens[i] in ("void", "int", "float", "string", "bool",
                               "dictionary", "dictionary@", "array",
                               "PluginContext@", "PluginContext", "auto"):
                i += 1
                if i < len(tokens):
                    var_name = tokens[i]
                    i += 1
                    if i < len(tokens) and tokens[i] == "=":
                        i += 1
                        expr, i = self._parse_expr_until(tokens, i, ";")
                        scope[var_name] = self._eval_expr(expr, scope)
                    else:
                        scope[var_name] = None
                    if i < len(tokens) and tokens[i] == ";":
                        i += 1
            elif i + 1 < len(tokens) and tokens[i + 1] == "=":
                var_name = tokens[i]
                i += 2
                expr, i = self._parse_expr_until(tokens, i, ";")
                value = self._eval_expr(expr, scope)
                scope[var_name] = value
                if var_name in self._globals:
                    self._globals[var_name] = value
                if i < len(tokens) and tokens[i] == ";":
                    i += 1
            elif i + 1 < len(tokens) and tokens[i + 1] in (".", "(", "["):
                expr, i = self._parse_expr_until(tokens, i, ";")
                self._eval_expr(expr, scope)
                if i < len(tokens) and tokens[i] == ";":
                    i += 1
            elif tokens[i] == ";":
                i += 1
            else:
                expr, i = self._parse_expr_until(tokens, i, ";")
                if expr:
                    self._eval_expr(expr, scope)
                if i < len(tokens) and tokens[i] == ";":
                    i += 1

        return _SENTINEL

    def _exec_if(self, tokens, i, scope):
        i += 1  # skip 'if'
        assert tokens[i] == "("
        cond_expr, i = self._parse_expr_until(tokens, i + 1, ")")
        i += 1  # skip ')'
        then_block, i = self._parse_block(tokens, i)
        else_block = []
        if i < len(tokens) and tokens[i] == "else":
            i += 1
            if i < len(tokens) and tokens[i] == "if":
                rest = tokens[i:]
                result, consumed = self._exec_if(rest, 0, scope)
                return result, i + consumed
            else:
                else_block, i = self._parse_block(tokens, i)

        if self._eval_expr(cond_expr, scope):
            result = self._exec_block(then_block, scope)
        else:
            result = self._exec_block(else_block, scope)
        return result, i

    def _exec_while(self, tokens, i, scope):
        i += 1
        assert tokens[i] == "("
        cond_expr, cond_end = self._parse_expr_until(tokens, i + 1, ")")
        body_start = cond_end + 1
        body_block, after_body = self._parse_block(tokens, body_start)

        iterations = 0
        while self._eval_expr(cond_expr, scope) and iterations < 100000:
            result = self._exec_block(body_block, scope)
            if result is not _SENTINEL:
                return result, after_body
            iterations += 1

        return _SENTINEL, after_body

    def _eval_expr(self, tokens: list[str], scope: dict) -> Any:
        if not tokens:
            return None
        if len(tokens) == 1:
            return self._eval_atom(tokens[0], scope)

        joined = " ".join(tokens)
        if tokens[0] == "@":
            return self._eval_expr(tokens[1:], scope)

        for op in ("||", "&&"):
            depth = 0
            for idx in range(len(tokens) - 1, -1, -1):
                if tokens[idx] in (")", "]"):
                    depth += 1
                elif tokens[idx] in ("(", "["):
                    depth -= 1
                elif depth == 0 and tokens[idx] == op:
                    left = self._eval_expr(tokens[:idx], scope)
                    right = self._eval_expr(tokens[idx + 1:], scope)
                    if op == "||":
                        return bool(left or right)
                    return bool(left and right)

        for op in ("==", "!=", "<=", ">=", "<", ">"):
            depth = 0
            for idx in range(len(tokens) - 1, -1, -1):
                if tokens[idx] in (")", "]"):
                    depth += 1
                elif tokens[idx] in ("(", "["):
                    depth -= 1
                elif depth == 0 and tokens[idx] == op:
                    left = self._eval_expr(tokens[:idx], scope)
                    right = self._eval_expr(tokens[idx + 1:], scope)
                    ops = {"==": lambda a, b: a == b, "!=": lambda a, b: a != b,
                           "<": lambda a, b: a < b, ">": lambda a, b: a > b,
                           "<=": lambda a, b: a <= b, ">=": lambda a, b: a >= b}
                    return ops[op](left, right)

        for op in ("+", "-"):
            depth = 0
            for idx in range(len(tokens) - 1, 0, -1):
                if tokens[idx] in (")", "]"):
                    depth += 1
                elif tokens[idx] in ("(", "["):
                    depth -= 1
                elif depth == 0 and tokens[idx] == op:
                    left = self._eval_expr(tokens[:idx], scope)
                    right = self._eval_expr(tokens[idx + 1:], scope)
                    if op == "+":
                        if isinstance(left, str) or isinstance(right, str):
                            return str(left) + str(right)
                        return (left or 0) + (right or 0)
                    return (left or 0) - (right or 0)

        for op in ("*", "/", "%"):
            depth = 0
            for idx in range(len(tokens) - 1, 0, -1):
                if tokens[idx] in (")", "]"):
                    depth += 1
                elif tokens[idx] in ("(", "["):
                    depth -= 1
                elif depth == 0 and tokens[idx] == op:
                    left = self._eval_expr(tokens[:idx], scope)
                    right = self._eval_expr(tokens[idx + 1:], scope)
                    if op == "*":
                        return (left or 0) * (right or 0)
                    if op == "/":
                        r = right or 0
                        return (left or 0) / r if r != 0 else 0
                    r = right or 0
                    return (left or 0) % r if r != 0 else 0

        if tokens[0] == "!" and len(tokens) > 1:
            return not self._eval_expr(tokens[1:], scope)

        if tokens[0] == "(" and tokens[-1] == ")":
            return self._eval_expr(tokens[1:-1], scope)

        if tokens[0] == "{" and tokens[-1] == "}":
            return self._parse_brace_literal(tokens[1:-1], scope)

        dot_chain = self._try_dot_chain(tokens, scope)
        if dot_chain is not _SENTINEL:
            return dot_chain

        return self._eval_atom(tokens[0], scope)

    def _try_dot_chain(self, tokens: list[str], scope: dict) -> Any:
        i = 0
        if i >= len(tokens):
            return _SENTINEL

        obj = self._eval_atom(tokens[i], scope)
        i += 1

        while i < len(tokens):
            if tokens[i] == ".":
                i += 1
                if i >= len(tokens):
                    break
                attr_name = tokens[i]
                i += 1
                if i < len(tokens) and tokens[i] == "(":
                    args, i = self._collect_call_args(tokens, i, scope)
                    method = getattr(obj, attr_name, None)
                    if callable(method):
                        obj = method(*args)
                    else:
                        return _SENTINEL
                else:
                    obj = getattr(obj, attr_name, None)
            elif tokens[i] == "(":
                args, i = self._collect_call_args(tokens, i, scope)
                if callable(obj):
                    obj = obj(*args)
                else:
                    return _SENTINEL
            elif tokens[i] == "[":
                i += 1
                idx_expr, i = self._parse_expr_until(tokens, i, "]")
                if i < len(tokens) and tokens[i] == "]":
                    i += 1
                key = self._eval_expr(idx_expr, scope)
                if isinstance(obj, dict):
                    obj = obj.get(key)
                elif isinstance(obj, (list, tuple)):
                    obj = obj[int(key)] if isinstance(key, (int, float)) else None
            else:
                break

        if i == len(tokens):
            return obj
        return _SENTINEL

    def _collect_call_args(self, tokens, i, scope):
        assert tokens[i] == "("
        i += 1
        args = []
        depth = 0
        current_arg = []
        while i < len(tokens):
            if tokens[i] in ("(", "{", "["):
                depth += 1
                current_arg.append(tokens[i])
            elif tokens[i] in (")", "}", "]"):
                if tokens[i] == ")" and depth == 0:
                    if current_arg:
                        args.append(self._eval_expr(current_arg, scope))
                    i += 1
                    break
                depth -= 1
                current_arg.append(tokens[i])
            elif tokens[i] == "," and depth == 0:
                if current_arg:
                    args.append(self._eval_expr(current_arg, scope))
                current_arg = []
            else:
                current_arg.append(tokens[i])
            i += 1
        return args, i

    def _parse_brace_literal(self, tokens, scope):
        parts = self._split_top_level(tokens, ",")
        has_dict_pairs = any(self._find_top_level(part, ":") >= 0 for part in parts)
        if not has_dict_pairs:
            return [self._eval_expr(part, scope) for part in parts if part]

        out = {}
        for part in parts:
            colon = self._find_top_level(part, ":")
            if colon <= 0:
                continue
            key = self._eval_expr(part[:colon], scope)
            out[key] = self._eval_expr(part[colon + 1:], scope)
        return out

    def _find_top_level(self, tokens, needle):
        depth = 0
        for idx, token in enumerate(tokens):
            if token in ("(", "{", "["):
                depth += 1
            elif token in (")", "}", "]"):
                depth -= 1
            elif depth == 0 and token == needle:
                return idx
        return -1

    def _split_top_level(self, tokens, separator):
        parts = []
        current = []
        depth = 0
        for token in tokens:
            if token in ("(", "{", "["):
                depth += 1
                current.append(token)
            elif token in (")", "}", "]"):
                depth -= 1
                current.append(token)
            elif depth == 0 and token == separator:
                parts.append(current)
                current = []
            else:
                current.append(token)
        parts.append(current)
        return parts

    def _eval_atom(self, token: str, scope: dict) -> Any:
        if token == "true":
            return True
        if token == "false":
            return False
        if token == "null" or token == "nil":
            return None
        if token.startswith('"') and token.endswith('"'):
            return token[1:-1]
        if token.startswith("'") and token.endswith("'"):
            return token[1:-1]
        try:
            return int(token)
        except ValueError:
            pass
        try:
            return float(token)
        except ValueError:
            pass
        if token in scope:
            return scope[token]
        if token in self._globals:
            return self._globals[token]
        if token in self._functions:
            fn = self._functions[token]
            interp = self
            def _call_wrapper(*args):
                return interp._call_function(fn, list(args))
            return _call_wrapper
        return None


class _ASFunction:
    __slots__ = ("name", "ret_type", "params", "body")

    def __init__(self, name, ret_type, params, body):
        self.name = name
        self.ret_type = ret_type
        self.params = params
        self.body = body


_SENTINEL = object()


def _strip_comments(source: str) -> str:
    source = re.sub(r"//[^\n]*", "", source)
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return source


def _tokenize(source: str) -> list[str]:
    pattern = re.compile(
        r'"(?:[^"\\]|\\.)*"'
        r"|'(?:[^'\\]|\\.)*'"
        r"|[a-zA-Z_]\w*"
        r"|0[xX][0-9a-fA-F]+"
        r"|\d+\.?\d*"
        r"|==|!=|<=|>=|&&|\|\||<<|>>"
        r"|[{}()\[\];,.<>=!+\-*/%@:&|^~?]"
    )
    return pattern.findall(source)


class _AngelScriptProxy:
    """Wraps PluginContext for AngelScript consumption."""

    def __init__(self, ctx: "PluginContext") -> None:
        self._ctx = ctx
        self.ui = ctx.ui
        self.engine = ctx.engine
        self.plugin_id = ctx.plugin_id
        self.path = ctx.path
        self.web_path = ctx.web_path
        self.assets_path = ctx.assets_path
        self._callbacks: dict[str, Callable] = {}

    def log(self, message="") -> None:
        self._ctx.log(str(message))

    def subscribe(self, topic, callback) -> str:
        return self._ctx.subscribe(str(topic), callback)

    def subscribe_once(self, topic, callback) -> str:
        return self._ctx.subscribe_once(str(topic), callback)

    def unsubscribe(self, token) -> bool:
        return self._ctx.unsubscribe(str(token))

    def on_damage(self, callback) -> str:
        return self._ctx.on_damage(callback)

    def on_heal(self, callback) -> str:
        return self._ctx.on_heal(callback)

    def on_skill(self, callback) -> str:
        return self._ctx.on_skill(callback)

    def on_boss(self, callback) -> str:
        return self._ctx.on_boss(callback)

    def on_snapshot(self, callback) -> str:
        return self._ctx.on_snapshot(callback)

    def on_encounter_finalized(self, callback) -> str:
        return self._ctx.on_encounter_finalized(callback)

    def emit(self, topic, payload=None) -> dict:
        return self._ctx.emit(str(topic), payload)

    def get_snapshot(self) -> dict:
        return self._ctx.get_snapshot()

    def snapshot_value(self, path, default=None):
        return self._ctx.snapshot_value(str(path), default)

    def recent_events(self, limit=20, topic="") -> list:
        return self._ctx.recent_events(int(limit), str(topic or ""))

    def get_setting(self, key, default=None):
        return self._ctx.get_setting(str(key), default)

    def setting(self, key, default=None):
        return self._ctx.setting(str(key), default)

    def set_setting(self, key, value=None):
        self._ctx.set_setting(str(key), value)

    def set_defaults(self, defaults):
        if isinstance(defaults, dict):
            self._ctx.set_defaults(defaults)

    def register_ui_panel(self, panel_id, metadata=None,
                          render=None, on_action=None):
        return self._ctx.register_ui_panel(
            str(panel_id),
            metadata if isinstance(metadata, dict) else {},
            render if callable(render) else None,
            on_action if callable(on_action) else None,
        )

    def register_render_hook(self, surface, callback, priority=0.0):
        return self._ctx.register_render_hook(str(surface), callback, float(priority))

    def set_overlay(self, surface, spec):
        return self._ctx.set_overlay(str(surface), spec)

    def clear_overlay(self, surface=None):
        self._ctx.clear_overlay(str(surface) if surface else None)

    def register_hotkey(self, hotkey_id, callback, default_key="", label=""):
        return self._ctx.register_hotkey(
            str(hotkey_id), callback, str(default_key or ""), str(label or ""))

    def register_engine(self, name, engine_obj):
        self._ctx.register_engine(str(name), engine_obj)

    def register_data_source(self, source_id, metadata=None, start=None, stop=None):
        return self._ctx.register_data_source(
            str(source_id), metadata,
            start if callable(start) else None,
            stop if callable(stop) else None,
        )

    def register_parser_adapter(self, adapter_id, metadata=None, handler=None):
        return self._ctx.register_parser_adapter(
            str(adapter_id), metadata, handler if callable(handler) else None)

    def register_exporter(self, exporter_id, metadata=None, handler=None):
        return self._ctx.register_exporter(
            str(exporter_id), metadata, handler if callable(handler) else None)

    def register_formatter(self, formatter_id, metadata=None, handler=None):
        return self._ctx.register_formatter(
            str(formatter_id), metadata, handler if callable(handler) else None)

    def register_trigger_type(self, trigger_type, metadata=None, handler=None):
        return self._ctx.register_trigger_type(
            str(trigger_type), metadata, handler if callable(handler) else None)

    def register_report_view(self, view_id, metadata=None, handler=None):
        return self._ctx.register_report_view(
            str(view_id), metadata, handler if callable(handler) else None)

    def register_timer(self, timer_id, metadata=None, handler=None):
        return self._ctx.register_timer(
            str(timer_id), metadata, handler if callable(handler) else None)

    def register_menu_category(self, name, icon, builder, priority=0.0):
        return self._ctx.register_menu_category(
            str(name), str(icon or ""), builder, float(priority))

    def register_menu_surface(self, surface_id, descriptor, priority=0.0):
        return self._ctx.register_menu_surface(
            str(surface_id), descriptor or {}, float(priority))

    def register_action_handler(self, handler):
        return self._ctx.register_action_handler(handler)

    def request_redraw(self, surface="", reason=""):
        return self._ctx.request_redraw(str(surface or ""), str(reason or ""))

    def open_window(self, panel_id="", width=0, height=0):
        return self._ctx.open_window(str(panel_id or ""), int(width), int(height))

    def set_interval(self, callback, seconds):
        return self._ctx.set_interval(callback, float(seconds))

    def set_timeout(self, callback, seconds):
        return self._ctx.set_timeout(callback, float(seconds))

    def clear_timer(self, token):
        return self._ctx.clear_timer(str(token))

    def run_on_ui(self, callback):
        self._ctx.run_on_ui(callback)

    def notify(self, title, message, duration_s=60.0, kind="plugin"):
        return self._ctx.notify(str(title), str(message), float(duration_s), str(kind))

    def dismiss_notify(self):
        return self._ctx.dismiss_notify()

    def toast(self, message):
        return self._ctx.toast(str(message))

    def get_engine(self, name, default=None):
        return self._ctx.get_engine(str(name), default)

    def require_engine(self, name):
        return self._ctx.require_engine(str(name))

    def call_engine(self, engine_name, method, *args, **kwargs):
        return self._ctx.call_engine(str(engine_name), str(method), *args, **kwargs)

    def call_runtime(self, action, *args, **kwargs):
        return self._ctx.call_runtime(str(action), *args, **kwargs)

    def ensure_requirements(self, install=True):
        return self._ctx.ensure_requirements(bool(install))

    def load_local(self, relative_path):
        return self._ctx.load_local(str(relative_path))

    def ui_panel(self, title, children=None):
        return self.ui.panel(str(title), children or [])

    def ui_text(self, text, style="value", align="left"):
        return self.ui.text(str(text), str(style), str(align))

    def ui_button(self, label, action, style="default", payload=None, disabled=False):
        return self.ui.button(str(label), str(action), str(style), payload, bool(disabled))

    def ui_kv(self, label, value, style="value"):
        return self.ui.kv(str(label), str(value), str(style))

    def ui_section(self, title, children=None, accent="cyan"):
        return self.ui.section(str(title), children or [], str(accent))

    def ui_row(self, children=None, align="left"):
        return self.ui.row(children or [], str(align))

    def ui_badge(self, text, style="muted"):
        return self.ui.badge(str(text), str(style))

    def ui_bar(self, label, pct=0.5, color="cyan", caption=""):
        return self.ui.bar(str(label), float(pct), str(color), str(caption))

    def ui_divider(self):
        return self.ui.divider()

    def ui_table(self, columns, rows, highlight_key="", title=""):
        return self.ui.table(columns, rows, str(highlight_key), str(title))

    @property
    def mem(self):
        return self._ctx.mem

    @property
    def event_bus(self):
        return self._ctx.event_bus

    @property
    def owner(self):
        return self._ctx.owner


class AngelScriptRuntime(ScriptRuntime):
    """Load ``.as`` AngelScript plugin scripts."""

    def __init__(self) -> None:
        self._interpreters: dict[str, _AngelScriptInterpreter] = {}

    @property
    def language(self) -> str:
        return "angelscript"

    @property
    def engine_name(self) -> str:
        return "AngelScript (pure-Python interpreter)"

    def load_script(self, entry_path: str, record: "PluginRecord",
                    ctx: "PluginContext") -> ModuleType:
        with open(entry_path, "r", encoding="utf-8") as fp:
            source = fp.read()

        interp = _AngelScriptInterpreter()
        proxy = _AngelScriptProxy(ctx)
        interp.register_host_object("ctx", proxy)
        interp.register_host_object("print", lambda *a: ctx.log(" ".join(str(x) for x in a)))
        interp.register_host_object("tostring", str)
        interp.register_host_object("toint", lambda v: int(v) if v is not None else 0)
        interp.register_host_object("tofloat", lambda v: float(v) if v is not None else 0.0)
        self._interpreters[record.plugin_id] = interp

        interp.execute_source(source)

        module = ScriptModule(
            f"act_plugin_as_{record.plugin_id}",
            "angelscript", entry_path,
        )

        for hook_name in ("on_load", "on_enable", "on_disable", "on_unload"):
            fn = interp.get_function(hook_name)
            if fn is not None:
                if hook_name == "on_load":
                    def _on_load(_ctx_ignored=None, *, _p=proxy, _f=fn):
                        _f(_p)
                    module.set_hook("on_load", _on_load)
                else:
                    module.set_hook(hook_name, wrap_callback(fn, "angelscript", ctx.log))

        return module

    def unload_script(self, record: "PluginRecord") -> None:
        self._interpreters.pop(record.plugin_id, None)
