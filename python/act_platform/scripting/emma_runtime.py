# -*- coding: utf-8 -*-
"""Emma scripting runtime — lightweight embedded scripting language.

Emma is a minimal, zero-dependency scripting language designed for
plugin authoring. It has Python-like readability with explicit block
delimiters, and direct access to the full platform SDK.

Plugin entry is an ``.emma`` file::

    -- plugin.emma

    let timer_token = ""

    fn on_load(ctx)
        ctx.log("Hello from Emma!")
        ctx.register_ui_panel("emma_panel",
            {title: "Emma Panel", description: "A demo panel"},
            render, on_action)
    end

    fn render(payload)
        return ctx.ui.panel("Emma Demo", [
            ctx.ui.text("Hello from Emma!"),
            ctx.ui.kv("Status", "Running"),
            ctx.ui.button("Click Me", "click"),
        ])
    end

    fn on_action(action_id, payload)
        ctx.log("action: " + action_id)
        return {ok: true}
    end

    fn on_unload()
        ctx.log("Bye from Emma!")
    end

Syntax summary:
    -- line comment
    let name = expr            variable declaration
    name = expr                assignment
    fn name(args) ... end      function
    if expr ... elif ... else ... end
    while expr ... end
    for name in expr ... end
    return expr
    expr.attr                  attribute access
    expr.method(args)          method call
    expr[key]                  index access
    [a, b, c]                  array literal
    {k: v, k2: v2}            dict literal
    "string" or 'string'      string literal
    true false nil             constants
    + - * / % == != < > <= >= and or not
    ..                         string concatenation
"""

from __future__ import annotations

import os
import re
from types import ModuleType
from typing import Any, Callable, Optional, TYPE_CHECKING

from .base import ScriptModule, ScriptRuntime, wrap_callback, resolve_local_script_path

if TYPE_CHECKING:
    from act_platform.plugins import PluginContext, PluginRecord


# ── tokenizer ──────────────────────────────────────────────────────────

_TOKEN_RE = re.compile(r"""
    (?P<string>"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')  |
    (?P<comment>--[^\n]*)                              |
    (?P<number>0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?)  |
    (?P<ident>[a-zA-Z_]\w*)                            |
    (?P<op>\.\.|\.\.|==|!=|<=|>=|=>|[{}()\[\];,.<>=!+\-*/%:])  |
    (?P<nl>\n)                                         |
    (?P<ws>[ \t\r]+)
""", re.VERBOSE)

_KEYWORDS = frozenset((
    "let", "fn", "end", "if", "elif", "else", "while", "for", "in",
    "return", "and", "or", "not", "true", "false", "nil", "break", "continue",
))


class _Token:
    __slots__ = ("kind", "value", "line")

    def __init__(self, kind: str, value: str, line: int) -> None:
        self.kind = kind
        self.value = value
        self.line = line

    def __repr__(self):
        return f"Token({self.kind}, {self.value!r}, L{self.line})"


def _tokenize(source: str) -> list[_Token]:
    tokens = []
    line = 1
    for m in _TOKEN_RE.finditer(source):
        if m.group("comment") or m.group("ws"):
            continue
        if m.group("nl"):
            line += 1
            continue
        if m.group("string"):
            tokens.append(_Token("STRING", m.group("string"), line))
        elif m.group("number"):
            tokens.append(_Token("NUMBER", m.group("number"), line))
        elif m.group("ident"):
            val = m.group("ident")
            kind = "KEYWORD" if val in _KEYWORDS else "IDENT"
            tokens.append(_Token(kind, val, line))
        elif m.group("op"):
            tokens.append(_Token("OP", m.group("op"), line))
    tokens.append(_Token("EOF", "", line))
    return tokens


# ── AST nodes ──────────────────────────────────────────────────────────

class _Node:
    pass


class _Literal(_Node):
    __slots__ = ("value",)

    def __init__(self, value): self.value = value


class _Ident(_Node):
    __slots__ = ("name",)

    def __init__(self, name): self.name = name


class _BinOp(_Node):
    __slots__ = ("op", "left", "right")

    def __init__(self, op, left, right):
        self.op = op
        self.left = left
        self.right = right


class _UnaryOp(_Node):
    __slots__ = ("op", "expr")

    def __init__(self, op, expr):
        self.op = op
        self.expr = expr


class _Call(_Node):
    __slots__ = ("func", "args")

    def __init__(self, func, args):
        self.func = func
        self.args = args


class _Attr(_Node):
    __slots__ = ("obj", "attr")

    def __init__(self, obj, attr):
        self.obj = obj
        self.attr = attr


class _Index(_Node):
    __slots__ = ("obj", "key")

    def __init__(self, obj, key):
        self.obj = obj
        self.key = key


class _ArrayLit(_Node):
    __slots__ = ("items",)

    def __init__(self, items): self.items = items


class _DictLit(_Node):
    __slots__ = ("pairs",)

    def __init__(self, pairs): self.pairs = pairs


class _Assign(_Node):
    __slots__ = ("target", "value")

    def __init__(self, target, value):
        self.target = target
        self.value = value


class _Let(_Node):
    __slots__ = ("name", "value")

    def __init__(self, name, value):
        self.name = name
        self.value = value


class _FnDef(_Node):
    __slots__ = ("name", "params", "body")

    def __init__(self, name, params, body):
        self.name = name
        self.params = params
        self.body = body


class _Return(_Node):
    __slots__ = ("expr",)

    def __init__(self, expr): self.expr = expr


class _If(_Node):
    __slots__ = ("branches", "else_body")

    def __init__(self, branches, else_body):
        self.branches = branches
        self.else_body = else_body


class _While(_Node):
    __slots__ = ("cond", "body")

    def __init__(self, cond, body):
        self.cond = cond
        self.body = body


class _For(_Node):
    __slots__ = ("var", "iter_expr", "body")

    def __init__(self, var, iter_expr, body):
        self.var = var
        self.iter_expr = iter_expr
        self.body = body


class _Break(_Node):
    pass


class _Continue(_Node):
    pass


class _ExprStmt(_Node):
    __slots__ = ("expr",)

    def __init__(self, expr): self.expr = expr


# ── parser ─────────────────────────────────────────────────────────────

class _Parser:
    def __init__(self, tokens: list[_Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    def _peek(self) -> _Token:
        return self._tokens[self._pos] if self._pos < len(self._tokens) else self._tokens[-1]

    def _advance(self) -> _Token:
        t = self._peek()
        self._pos += 1
        return t

    def _expect(self, kind: str = "", value: str = "") -> _Token:
        t = self._advance()
        if kind and t.kind != kind:
            raise SyntaxError(f"line {t.line}: expected {kind} {value!r}, got {t.kind} {t.value!r}")
        if value and t.value != value:
            raise SyntaxError(f"line {t.line}: expected {value!r}, got {t.value!r}")
        return t

    def _match(self, kind: str = "", value: str = "") -> Optional[_Token]:
        t = self._peek()
        if kind and t.kind != kind:
            return None
        if value and t.value != value:
            return None
        return self._advance()

    def parse(self) -> list[_Node]:
        stmts = []
        while self._peek().kind != "EOF":
            self._skip_semis()
            if self._peek().kind == "EOF":
                break
            stmts.append(self._stmt())
        return stmts

    def _skip_semis(self):
        while self._match("OP", ";"):
            pass

    def _stmt(self) -> _Node:
        t = self._peek()
        if t.kind == "KEYWORD":
            if t.value == "let":
                return self._let_stmt()
            if t.value == "fn":
                return self._fn_def()
            if t.value == "return":
                return self._return_stmt()
            if t.value == "if":
                return self._if_stmt()
            if t.value == "while":
                return self._while_stmt()
            if t.value == "for":
                return self._for_stmt()
            if t.value == "break":
                self._advance()
                self._match("OP", ";")
                return _Break()
            if t.value == "continue":
                self._advance()
                self._match("OP", ";")
                return _Continue()
        expr = self._expr()
        if self._match("OP", "="):
            value = self._expr()
            self._match("OP", ";")
            return _Assign(expr, value)
        self._match("OP", ";")
        return _ExprStmt(expr)

    def _let_stmt(self) -> _Node:
        self._expect("KEYWORD", "let")
        name = self._expect("IDENT").value
        value = _Literal(None)
        if self._match("OP", "="):
            value = self._expr()
        self._match("OP", ";")
        return _Let(name, value)

    def _fn_def(self) -> _Node:
        self._expect("KEYWORD", "fn")
        name = self._expect("IDENT").value
        self._expect("OP", "(")
        params = []
        if not self._match("OP", ")"):
            params.append(self._expect("IDENT").value)
            while self._match("OP", ","):
                params.append(self._expect("IDENT").value)
            self._expect("OP", ")")
        body = self._block("end")
        self._expect("KEYWORD", "end")
        return _FnDef(name, params, body)

    def _return_stmt(self) -> _Node:
        self._expect("KEYWORD", "return")
        if self._peek().kind == "KEYWORD" and self._peek().value in ("end", "else", "elif"):
            return _Return(None)
        if self._peek().kind == "OP" and self._peek().value == ";":
            self._advance()
            return _Return(None)
        expr = self._expr()
        self._match("OP", ";")
        return _Return(expr)

    def _if_stmt(self) -> _Node:
        self._expect("KEYWORD", "if")
        cond = self._expr()
        body = self._block("elif", "else", "end")
        branches = [(cond, body)]
        while self._match("KEYWORD", "elif"):
            c = self._expr()
            b = self._block("elif", "else", "end")
            branches.append((c, b))
        else_body = []
        if self._match("KEYWORD", "else"):
            else_body = self._block("end")
        self._expect("KEYWORD", "end")
        return _If(branches, else_body)

    def _while_stmt(self) -> _Node:
        self._expect("KEYWORD", "while")
        cond = self._expr()
        body = self._block("end")
        self._expect("KEYWORD", "end")
        return _While(cond, body)

    def _for_stmt(self) -> _Node:
        self._expect("KEYWORD", "for")
        var = self._expect("IDENT").value
        self._expect("KEYWORD", "in")
        iter_expr = self._expr()
        body = self._block("end")
        self._expect("KEYWORD", "end")
        return _For(var, iter_expr, body)

    def _block(self, *terminators) -> list[_Node]:
        stmts = []
        while True:
            self._skip_semis()
            t = self._peek()
            if t.kind == "EOF":
                break
            if t.kind == "KEYWORD" and t.value in terminators:
                break
            stmts.append(self._stmt())
        return stmts

    # ── expression parser (precedence climbing) ───────────────────────

    def _expr(self) -> _Node:
        return self._or_expr()

    def _or_expr(self) -> _Node:
        left = self._and_expr()
        while self._match("KEYWORD", "or"):
            right = self._and_expr()
            left = _BinOp("or", left, right)
        return left

    def _and_expr(self) -> _Node:
        left = self._not_expr()
        while self._match("KEYWORD", "and"):
            right = self._not_expr()
            left = _BinOp("and", left, right)
        return left

    def _not_expr(self) -> _Node:
        if self._match("KEYWORD", "not"):
            return _UnaryOp("not", self._not_expr())
        return self._cmp_expr()

    def _cmp_expr(self) -> _Node:
        left = self._concat_expr()
        while True:
            op = None
            for o in ("==", "!=", "<=", ">=", "<", ">"):
                if self._peek().kind == "OP" and self._peek().value == o:
                    op = o
                    self._advance()
                    break
            if op is None:
                break
            right = self._concat_expr()
            left = _BinOp(op, left, right)
        return left

    def _concat_expr(self) -> _Node:
        left = self._add_expr()
        while self._peek().kind == "OP" and self._peek().value == "..":
            self._advance()
            right = self._add_expr()
            left = _BinOp("..", left, right)
        return left

    def _add_expr(self) -> _Node:
        left = self._mul_expr()
        while self._peek().kind == "OP" and self._peek().value in ("+", "-"):
            op = self._advance().value
            right = self._mul_expr()
            left = _BinOp(op, left, right)
        return left

    def _mul_expr(self) -> _Node:
        left = self._unary_expr()
        while self._peek().kind == "OP" and self._peek().value in ("*", "/", "%"):
            op = self._advance().value
            right = self._unary_expr()
            left = _BinOp(op, left, right)
        return left

    def _unary_expr(self) -> _Node:
        if self._peek().kind == "OP" and self._peek().value == "-":
            self._advance()
            return _UnaryOp("-", self._unary_expr())
        if self._peek().kind == "OP" and self._peek().value == "!":
            self._advance()
            return _UnaryOp("not", self._unary_expr())
        return self._postfix_expr()

    def _postfix_expr(self) -> _Node:
        node = self._primary()
        while True:
            if self._peek().kind == "OP" and self._peek().value == ".":
                self._advance()
                attr = self._expect("IDENT").value
                if self._peek().kind == "OP" and self._peek().value == "(":
                    self._advance()
                    args = self._arglist()
                    node = _Call(_Attr(node, attr), args)
                else:
                    node = _Attr(node, attr)
            elif self._peek().kind == "OP" and self._peek().value == "(":
                self._advance()
                args = self._arglist()
                node = _Call(node, args)
            elif self._peek().kind == "OP" and self._peek().value == "[":
                self._advance()
                key = self._expr()
                self._expect("OP", "]")
                node = _Index(node, key)
            else:
                break
        return node

    def _arglist(self) -> list[_Node]:
        args = []
        if self._peek().kind != "OP" or self._peek().value != ")":
            args.append(self._expr())
            while self._match("OP", ","):
                args.append(self._expr())
        self._expect("OP", ")")
        return args

    def _primary(self) -> _Node:
        t = self._peek()
        if t.kind == "NUMBER":
            self._advance()
            v = t.value
            if "." in v or "e" in v.lower():
                return _Literal(float(v))
            if v.startswith("0x") or v.startswith("0X"):
                return _Literal(int(v, 16))
            return _Literal(int(v))
        if t.kind == "STRING":
            self._advance()
            return _Literal(_unescape(t.value[1:-1]))
        if t.kind == "KEYWORD" and t.value == "true":
            self._advance()
            return _Literal(True)
        if t.kind == "KEYWORD" and t.value == "false":
            self._advance()
            return _Literal(False)
        if t.kind == "KEYWORD" and t.value == "nil":
            self._advance()
            return _Literal(None)
        if t.kind == "KEYWORD" and t.value == "fn":
            return self._lambda()
        if t.kind == "IDENT":
            self._advance()
            return _Ident(t.value)
        if t.kind == "OP" and t.value == "(":
            self._advance()
            expr = self._expr()
            self._expect("OP", ")")
            return expr
        if t.kind == "OP" and t.value == "[":
            return self._array_lit()
        if t.kind == "OP" and t.value == "{":
            return self._dict_lit()
        raise SyntaxError(f"line {t.line}: unexpected {t.kind} {t.value!r}")

    def _lambda(self) -> _Node:
        self._expect("KEYWORD", "fn")
        self._expect("OP", "(")
        params = []
        if not self._match("OP", ")"):
            params.append(self._expect("IDENT").value)
            while self._match("OP", ","):
                params.append(self._expect("IDENT").value)
            self._expect("OP", ")")
        body = self._block("end")
        self._expect("KEYWORD", "end")
        return _FnDef("", params, body)

    def _array_lit(self) -> _Node:
        self._expect("OP", "[")
        items = []
        if not (self._peek().kind == "OP" and self._peek().value == "]"):
            items.append(self._expr())
            while self._match("OP", ","):
                if self._peek().kind == "OP" and self._peek().value == "]":
                    break
                items.append(self._expr())
        self._expect("OP", "]")
        return _ArrayLit(items)

    def _dict_lit(self) -> _Node:
        self._expect("OP", "{")
        pairs = []
        if not (self._peek().kind == "OP" and self._peek().value == "}"):
            k, v = self._dict_entry()
            pairs.append((k, v))
            while self._match("OP", ","):
                if self._peek().kind == "OP" and self._peek().value == "}":
                    break
                k, v = self._dict_entry()
                pairs.append((k, v))
        self._expect("OP", "}")
        return _DictLit(pairs)

    def _dict_entry(self) -> tuple[_Node, _Node]:
        t = self._peek()
        if t.kind == "IDENT" and self._pos + 1 < len(self._tokens) and self._tokens[self._pos + 1].value == ":":
            key = _Literal(self._advance().value)
            self._expect("OP", ":")
        elif t.kind == "STRING":
            key = _Literal(_unescape(self._advance().value[1:-1]))
            self._expect("OP", ":")
        else:
            key = self._expr()
            self._expect("OP", ":")
        value = self._expr()
        return key, value


def _unescape(s: str) -> str:
    return s.replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\").replace('\\"', '"').replace("\\'", "'")


# ── interpreter ────────────────────────────────────────────────────────

class _ReturnSignal(Exception):
    __slots__ = ("value",)

    def __init__(self, value): self.value = value


class _BreakSignal(Exception):
    pass


class _ContinueSignal(Exception):
    pass


class _EmmaScope:
    __slots__ = ("vars", "parent")

    def __init__(self, parent: Optional["_EmmaScope"] = None):
        self.vars: dict[str, Any] = {}
        self.parent = parent

    def get(self, name: str) -> Any:
        if name in self.vars:
            return self.vars[name]
        if self.parent is not None:
            return self.parent.get(name)
        return None

    def set(self, name: str, value: Any) -> None:
        if name in self.vars:
            self.vars[name] = value
        elif self.parent is not None and self.parent.has(name):
            self.parent.set(name, value)
        else:
            self.vars[name] = value

    def has(self, name: str) -> bool:
        if name in self.vars:
            return True
        if self.parent is not None:
            return self.parent.has(name)
        return False

    def define(self, name: str, value: Any) -> None:
        self.vars[name] = value


class _EmmaCallable:
    __slots__ = ("name", "params", "body", "closure", "_interp")

    def __init__(self, name, params, body, closure):
        self.name = name
        self.params = params
        self.body = body
        self.closure = closure
        self._interp: Optional["_EmmaInterpreter"] = None

    def __call__(self, *args):
        if self._interp is not None:
            return self._interp._call(self, list(args))
        raise RuntimeError("Emma callable not bound to interpreter")


class _EmmaInterpreter:
    def __init__(self) -> None:
        self._global = _EmmaScope()
        self._builtins()

    def _builtins(self):
        self._global.define("print", lambda *a: print(*a))
        self._global.define("str", lambda v: str(v) if v is not None else "nil")
        self._global.define("int", lambda v: int(v) if v is not None else 0)
        self._global.define("float", lambda v: float(v) if v is not None else 0.0)
        self._global.define("len", lambda v: len(v) if v is not None else 0)
        self._global.define("type", lambda v: type(v).__name__)
        self._global.define("tostring", lambda v: str(v) if v is not None else "nil")
        self._global.define("tonumber", lambda v: float(v) if v is not None else 0.0)
        self._global.define("pairs", lambda d: list(d.items()) if isinstance(d, dict) else [])
        self._global.define("ipairs", lambda a: list(enumerate(a)) if isinstance(a, list) else [])
        self._global.define("range", range)
        self._global.define("abs", abs)
        self._global.define("min", min)
        self._global.define("max", max)

    def register(self, name: str, value: Any) -> None:
        self._global.define(name, value)

    def execute(self, stmts: list[_Node]) -> None:
        self._exec_stmts(stmts, self._global)

    def get_function(self, name: str) -> Optional[Callable]:
        val = self._global.get(name)
        if isinstance(val, _EmmaCallable):
            interp = self

            def _caller(*args):
                return interp._call(val, list(args))
            _caller.__name__ = name
            return _caller
        if callable(val):
            return val
        return None

    def _exec_stmts(self, stmts: list[_Node], scope: _EmmaScope) -> None:
        for stmt in stmts:
            self._exec(stmt, scope)

    def _exec(self, node: _Node, scope: _EmmaScope) -> None:
        if isinstance(node, _Let):
            scope.define(node.name, self._eval(node.value, scope))
        elif isinstance(node, _FnDef):
            fn = _EmmaCallable(node.name, node.params, node.body, scope)
            fn._interp = self
            if node.name:
                scope.define(node.name, fn)
        elif isinstance(node, _Assign):
            value = self._eval(node.value, scope)
            self._assign(node.target, value, scope)
        elif isinstance(node, _Return):
            raise _ReturnSignal(self._eval(node.expr, scope) if node.expr else None)
        elif isinstance(node, _If):
            for cond, body in node.branches:
                if self._truthy(self._eval(cond, scope)):
                    self._exec_stmts(body, scope)
                    return
            if node.else_body:
                self._exec_stmts(node.else_body, scope)
        elif isinstance(node, _While):
            iters = 0
            while self._truthy(self._eval(node.cond, scope)) and iters < 1_000_000:
                try:
                    self._exec_stmts(node.body, scope)
                except _BreakSignal:
                    break
                except _ContinueSignal:
                    pass
                iters += 1
        elif isinstance(node, _For):
            iterable = self._eval(node.iter_expr, scope)
            if iterable is None:
                return
            try:
                items = iter(iterable)
            except TypeError:
                return
            for item in items:
                scope.define(node.var, item)
                try:
                    self._exec_stmts(node.body, scope)
                except _BreakSignal:
                    break
                except _ContinueSignal:
                    pass
        elif isinstance(node, _Break):
            raise _BreakSignal()
        elif isinstance(node, _Continue):
            raise _ContinueSignal()
        elif isinstance(node, _ExprStmt):
            self._eval(node.expr, scope)

    def _eval(self, node: _Node, scope: _EmmaScope) -> Any:
        if node is None:
            return None
        if isinstance(node, _Literal):
            return node.value
        if isinstance(node, _Ident):
            return scope.get(node.name)
        if isinstance(node, _BinOp):
            return self._eval_binop(node, scope)
        if isinstance(node, _UnaryOp):
            val = self._eval(node.expr, scope)
            if node.op == "-":
                return -(val or 0)
            if node.op == "not":
                return not self._truthy(val)
        if isinstance(node, _Attr):
            obj = self._eval(node.obj, scope)
            if obj is None:
                return None
            if isinstance(obj, dict):
                return obj.get(node.attr)
            return getattr(obj, node.attr, None)
        if isinstance(node, _Index):
            obj = self._eval(node.obj, scope)
            key = self._eval(node.key, scope)
            if isinstance(obj, dict):
                return obj.get(key)
            if isinstance(obj, (list, tuple)):
                try:
                    return obj[int(key)]
                except (IndexError, TypeError, ValueError):
                    return None
            return None
        if isinstance(node, _Call):
            return self._eval_call(node, scope)
        if isinstance(node, _ArrayLit):
            return [self._eval(item, scope) for item in node.items]
        if isinstance(node, _DictLit):
            return {self._eval(k, scope): self._eval(v, scope)
                    for k, v in node.pairs}
        if isinstance(node, _FnDef):
            fn = _EmmaCallable(node.name, node.params, node.body, scope)
            fn._interp = self
            return fn
        return None

    def _eval_binop(self, node: _BinOp, scope: _EmmaScope) -> Any:
        if node.op == "and":
            left = self._eval(node.left, scope)
            return self._eval(node.right, scope) if self._truthy(left) else left
        if node.op == "or":
            left = self._eval(node.left, scope)
            return left if self._truthy(left) else self._eval(node.right, scope)

        left = self._eval(node.left, scope)
        right = self._eval(node.right, scope)

        if node.op == "+":
            if isinstance(left, str) or isinstance(right, str):
                return str(left if left is not None else "") + str(right if right is not None else "")
            return (left or 0) + (right or 0)
        if node.op == "..":
            return str(left if left is not None else "") + str(right if right is not None else "")
        if node.op == "-":
            return (left or 0) - (right or 0)
        if node.op == "*":
            return (left or 0) * (right or 0)
        if node.op == "/":
            r = right or 0
            return (left or 0) / r if r != 0 else 0
        if node.op == "%":
            r = right or 0
            return (left or 0) % r if r != 0 else 0
        if node.op == "==":
            return left == right
        if node.op == "!=":
            return left != right
        if node.op == "<":
            return (left or 0) < (right or 0)
        if node.op == ">":
            return (left or 0) > (right or 0)
        if node.op == "<=":
            return (left or 0) <= (right or 0)
        if node.op == ">=":
            return (left or 0) >= (right or 0)
        return None

    def _eval_call(self, node: _Call, scope: _EmmaScope) -> Any:
        func = self._eval(node.func, scope)
        args = [self._eval(a, scope) for a in node.args]
        if isinstance(func, _EmmaCallable):
            return self._call(func, args)
        if callable(func):
            return func(*args)
        return None

    def _call(self, fn: _EmmaCallable, args: list) -> Any:
        call_scope = _EmmaScope(fn.closure)
        for i, p in enumerate(fn.params):
            call_scope.define(p, args[i] if i < len(args) else None)
        try:
            self._exec_stmts(fn.body, call_scope)
        except _ReturnSignal as r:
            return r.value
        return None

    def _assign(self, target: _Node, value: Any, scope: _EmmaScope) -> None:
        if isinstance(target, _Ident):
            scope.set(target.name, value)
        elif isinstance(target, _Attr):
            obj = self._eval(target.obj, scope)
            if isinstance(obj, dict):
                obj[target.attr] = value
            elif obj is not None:
                setattr(obj, target.attr, value)
        elif isinstance(target, _Index):
            obj = self._eval(target.obj, scope)
            key = self._eval(target.key, scope)
            if isinstance(obj, dict):
                obj[key] = value
            elif isinstance(obj, list):
                try:
                    obj[int(key)] = value
                except (IndexError, TypeError, ValueError):
                    pass

    def _truthy(self, val: Any) -> bool:
        if val is None or val is False or val == 0 or val == "":
            return False
        return True


# ── proxy ──────────────────────────────────────────────────────────────

class _EmmaProxy:
    """Wraps PluginContext for Emma consumption.

    Emma scripts call methods directly: ctx.log("msg"), ctx.ui.panel(...)
    """

    def __init__(self, ctx: "PluginContext") -> None:
        self._ctx = ctx
        self.ui = ctx.ui
        self.engine = ctx.engine
        self.plugin_id = ctx.plugin_id
        self.path = ctx.path
        self.web_path = ctx.web_path
        self.assets_path = ctx.assets_path

    def log(self, message=""):
        self._ctx.log(str(message))

    def subscribe(self, topic, callback):
        return self._ctx.subscribe(str(topic), callback)

    def subscribe_once(self, topic, callback):
        return self._ctx.subscribe_once(str(topic), callback)

    def unsubscribe(self, token):
        return self._ctx.unsubscribe(str(token))

    def on_damage(self, callback):
        return self._ctx.on_damage(callback)

    def on_heal(self, callback):
        return self._ctx.on_heal(callback)

    def on_skill(self, callback):
        return self._ctx.on_skill(callback)

    def on_boss(self, callback):
        return self._ctx.on_boss(callback)

    def on_snapshot(self, callback):
        return self._ctx.on_snapshot(callback)

    def on_encounter_finalized(self, callback):
        return self._ctx.on_encounter_finalized(callback)

    def emit(self, topic, payload=None):
        return self._ctx.emit(str(topic), payload)

    def get_snapshot(self):
        return self._ctx.get_snapshot()

    def snapshot_value(self, path, default=None):
        return self._ctx.snapshot_value(str(path), default)

    def time(self):
        return self._ctx.time()

    def recent_events(self, limit=20, topic=""):
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

    @property
    def mem(self):
        return self._ctx.mem

    @property
    def event_bus(self):
        return self._ctx.event_bus

    @property
    def owner(self):
        return self._ctx.owner


# ── runtime entry ──────────────────────────────────────────────────────

class EmmaRuntime(ScriptRuntime):
    """Load ``.emma`` plugin scripts via the built-in Emma interpreter."""

    def __init__(self) -> None:
        self._interpreters: dict[str, _EmmaInterpreter] = {}

    @property
    def language(self) -> str:
        return "emma"

    @property
    def engine_name(self) -> str:
        return "Emma (built-in interpreter)"

    def load_script(self, entry_path: str, record: "PluginRecord",
                    ctx: "PluginContext") -> ModuleType:
        with open(entry_path, "r", encoding="utf-8") as fp:
            source = fp.read()

        tokens = _tokenize(source)
        parser = _Parser(tokens)
        stmts = parser.parse()

        interp = _EmmaInterpreter()
        proxy = _EmmaProxy(ctx)
        interp.register("ctx", proxy)
        interp.register("log", lambda *a: ctx.log(" ".join(str(x) for x in a)))
        interp.register("json_encode", lambda v: __import__("json").dumps(v, default=str))
        interp.register("json_decode", lambda s: __import__("json").loads(s))
        interp.register("time", lambda: __import__("time").time())
        interp.register("sleep", lambda s: __import__("time").sleep(float(s)))

        plugin_base = os.path.abspath(str(record.path))

        def _emma_load_script(relative_path, *, _interp=interp, _base=plugin_base):
            target = resolve_local_script_path(_base, relative_path)
            with open(target, "r", encoding="utf-8") as fp:
                more_source = fp.read()
            more_tokens = _tokenize(more_source)
            more_stmts = _Parser(more_tokens).parse()
            _interp.execute(more_stmts)
            return True

        interp.register("load_script", _emma_load_script)
        interp.register("import", _emma_load_script)
        proxy.load_script = _emma_load_script

        self._interpreters[record.plugin_id] = interp

        interp.execute(stmts)

        module = ScriptModule(
            f"act_plugin_emma_{record.plugin_id}",
            "emma", entry_path,
        )

        for hook_name in ("on_load", "on_enable", "on_disable", "on_unload"):
            fn = interp.get_function(hook_name)
            if fn is not None:
                if hook_name == "on_load":
                    def _on_load(_ctx_ignored=None, *, _p=proxy, _f=fn):
                        _f(_p)
                    module.set_hook("on_load", _on_load)
                else:
                    module.set_hook(hook_name, wrap_callback(fn, "emma", ctx.log))

        return module

    def unload_script(self, record: "PluginRecord") -> None:
        self._interpreters.pop(record.plugin_id, None)
