"""Tool registry: declare, resolve, and execute tools callable by the LLM.

Follows the OpenAI function-calling wire schema so tools translate 1-to-1 into
the ``tools`` array of a chat-completions request.
"""

from __future__ import annotations

import inspect
import json
import traceback
from dataclasses import asdict, dataclass, field, is_dataclass
from typing import Any, Callable, Dict, List, Optional, Sequence


# ---------------------------------------------------------------------------
# Descriptor
# ---------------------------------------------------------------------------

@dataclass
class ToolDescriptor:
    name: str
    description: str
    parameters: Dict[str, Any]  # JSON Schema object
    handler: Callable[..., Any] = field(repr=False)
    category: str = "general"
    requires_confirm: bool = False
    tags: Dict[str, Any] = field(default_factory=dict)
    enabled: bool = True

    @property
    def input_schema(self) -> Dict[str, Any]:
        return self.parameters

    def to_openai_schema(self) -> Dict[str, Any]:
        return {
            "type": "function",
            "function": {
                "name": self.name,
                "description": self.description,
                "parameters": self.parameters,
            },
        }


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

class ToolRegistry:
    """Central hub that holds tool descriptors and dispatches calls."""

    def __init__(self) -> None:
        self._tools: Dict[str, ToolDescriptor] = {}
        self._openai_cache: Optional[List[Dict[str, Any]]] = None

    # -- Registration --

    def register(
        self,
        name: str,
        description: str,
        parameters: Dict[str, Any],
        handler: Callable[..., Any],
        category: str = "general",
        requires_confirm: bool = False,
        tags: Optional[Dict[str, Any]] = None,
    ) -> None:
        self._tools[name] = ToolDescriptor(
            name=name,
            description=description,
            parameters=parameters,
            handler=handler,
            category=category,
            requires_confirm=requires_confirm,
            tags=tags or {},
        )
        self._openai_cache = None

    def register_tool(self, desc: ToolDescriptor) -> None:
        self._tools[desc.name] = desc
        self._openai_cache = None

    def unregister(self, name: str) -> None:
        self._tools.pop(name, None)
        self._openai_cache = None

    # -- Decorator --

    def tool(
        self,
        name: Optional[str] = None,
        description: str = "",
        parameters: Optional[Dict[str, Any]] = None,
        category: str = "general",
        requires_confirm: bool = False,
        tags: Optional[Dict[str, Any]] = None,
    ) -> Callable:
        """Decorator to register a function as a tool."""
        def decorator(fn: Callable) -> Callable:
            tool_name = name or fn.__name__
            tool_desc = description or (fn.__doc__ or "").strip().split("\n")[0]
            tool_params = parameters or self._infer_parameters(fn)
            self.register(tool_name, tool_desc, tool_params, fn, category,
                          requires_confirm, tags)
            return fn
        return decorator

    # -- Query --

    def get(self, name: str) -> Optional[ToolDescriptor]:
        return self._tools.get(name)

    def list_tools(self, category: Optional[str] = None,
                    include_disabled: bool = False) -> List[ToolDescriptor]:
        tools = list(self._tools.values())
        if not include_disabled:
            tools = [t for t in tools if t.enabled]
        if category:
            tools = [t for t in tools if t.category == category]
        return sorted(tools, key=lambda t: (t.category, t.name))

    def to_openai_tools(self, category: Optional[str] = None) -> List[Dict[str, Any]]:
        if category is None and self._openai_cache is not None:
            return self._openai_cache
        result = [t.to_openai_schema() for t in self.list_tools(category)]
        if category is None:
            self._openai_cache = result
        return result

    def categories(self) -> List[str]:
        return sorted({t.category for t in self._tools.values()})

    # -- Execution --

    def execute(self, name: str, arguments: str | Dict[str, Any]) -> str:
        """Execute a tool by name, return result as JSON string."""
        desc = self._tools.get(name)
        if not desc:
            return json.dumps({"error": f"Unknown tool: {name}"}, ensure_ascii=False)

        ok, parsed = self._parse_arguments(desc, arguments)
        if not ok:
            return json.dumps({"error": parsed}, ensure_ascii=False)
        args = parsed

        try:
            result = self._invoke_handler(desc.handler, args)
            if isinstance(result, str):
                return result
            return json.dumps(
                self._normalize_result(result), ensure_ascii=False, default=str)
        except Exception:
            return json.dumps({
                "error": traceback.format_exc(limit=3),
            }, ensure_ascii=False)

    @staticmethod
    def _invoke_handler(handler: Callable[..., Any], args: Any) -> Any:
        if not isinstance(args, dict):
            return handler(args)
        try:
            sig = inspect.signature(handler)
        except (TypeError, ValueError):
            return handler(**args)

        params = list(sig.parameters.values())
        if not params:
            return handler()

        positional = [
            p for p in params
            if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
        ]
        has_varkw = any(p.kind == p.VAR_KEYWORD for p in params)
        keyword_only = [p for p in params if p.kind == p.KEYWORD_ONLY]

        if has_varkw or keyword_only:
            return handler(**args)

        if len(positional) == 1:
            param = positional[0]
            if param.kind == param.POSITIONAL_ONLY:
                return handler(args)
            if args and set(args.keys()).issubset({param.name}):
                return handler(**args)
            return handler(args)

        return handler(**args)

    @staticmethod
    def _normalize_result(result: Any) -> Any:
        if hasattr(result, "to_dict") and callable(getattr(result, "to_dict")):
            try:
                return result.to_dict()
            except Exception:
                pass
        if is_dataclass(result):
            return asdict(result)
        if hasattr(result, "content") and isinstance(getattr(result, "content"), list):
            return {"content": getattr(result, "content")}
        return result

    @staticmethod
    def _parse_arguments(
        desc: ToolDescriptor,
        arguments: str | Dict[str, Any],
    ) -> tuple[bool, Any]:
        if isinstance(arguments, str):
            raw = arguments.strip()
            if raw:
                try:
                    args: Any = json.loads(raw)
                except json.JSONDecodeError as exc:
                    return False, (
                        f"Invalid JSON arguments for {desc.name}: "
                        f"{exc.msg} at char {exc.pos}"
                    )
            else:
                args = {}
        elif isinstance(arguments, dict):
            args = dict(arguments)
        else:
            args = arguments

        schema = desc.parameters if isinstance(desc.parameters, dict) else {}
        if schema.get("type", "object") == "object":
            if not isinstance(args, dict):
                return False, f"Tool arguments for {desc.name} must be a JSON object"
            required = [str(k) for k in schema.get("required", []) if str(k)]
            missing = [key for key in required if key not in args or args[key] is None]
            if missing:
                return False, (
                    f"Missing required argument(s) for {desc.name}: "
                    + ", ".join(missing)
                )
        return True, args

    # -- Parameter inference --

    @staticmethod
    def _infer_parameters(fn: Callable) -> Dict[str, Any]:
        sig = inspect.signature(fn)
        props: Dict[str, Any] = {}
        required: List[str] = []
        type_map = {
            str: "string", int: "integer", float: "number",
            bool: "boolean", list: "array", dict: "object",
        }
        for pname, param in sig.parameters.items():
            ann = param.annotation
            ptype = type_map.get(ann, "string") if ann != inspect.Parameter.empty else "string"
            prop: Dict[str, Any] = {"type": ptype}
            if param.default is inspect.Parameter.empty:
                required.append(pname)
            else:
                prop["default"] = param.default
            props[pname] = prop
        schema: Dict[str, Any] = {
            "type": "object",
            "properties": props,
        }
        if required:
            schema["required"] = required
        return schema


# ---------------------------------------------------------------------------
# Global singleton
# ---------------------------------------------------------------------------

_global_registry: Optional[ToolRegistry] = None


def get_global_registry() -> ToolRegistry:
    global _global_registry
    if _global_registry is None:
        _global_registry = ToolRegistry()
    return _global_registry
