"""Tool registry: declare, resolve, and execute tools callable by the LLM.

Follows the OpenAI function-calling wire schema so tools translate 1-to-1 into
the ``tools`` array of a chat-completions request.
"""

from __future__ import annotations

import inspect
import json
import traceback
from dataclasses import dataclass, field
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

    # -- Registration --

    def register(
        self,
        name: str,
        description: str,
        parameters: Dict[str, Any],
        handler: Callable[..., Any],
        category: str = "general",
        requires_confirm: bool = False,
    ) -> None:
        self._tools[name] = ToolDescriptor(
            name=name,
            description=description,
            parameters=parameters,
            handler=handler,
            category=category,
            requires_confirm=requires_confirm,
        )

    def register_tool(self, desc: ToolDescriptor) -> None:
        self._tools[desc.name] = desc

    def unregister(self, name: str) -> None:
        self._tools.pop(name, None)

    # -- Decorator --

    def tool(
        self,
        name: Optional[str] = None,
        description: str = "",
        parameters: Optional[Dict[str, Any]] = None,
        category: str = "general",
        requires_confirm: bool = False,
    ) -> Callable:
        """Decorator to register a function as a tool."""
        def decorator(fn: Callable) -> Callable:
            tool_name = name or fn.__name__
            tool_desc = description or (fn.__doc__ or "").strip().split("\n")[0]
            tool_params = parameters or self._infer_parameters(fn)
            self.register(tool_name, tool_desc, tool_params, fn, category, requires_confirm)
            return fn
        return decorator

    # -- Query --

    def get(self, name: str) -> Optional[ToolDescriptor]:
        return self._tools.get(name)

    def list_tools(self, category: Optional[str] = None) -> List[ToolDescriptor]:
        tools = list(self._tools.values())
        if category:
            tools = [t for t in tools if t.category == category]
        return sorted(tools, key=lambda t: (t.category, t.name))

    def to_openai_tools(self, category: Optional[str] = None) -> List[Dict[str, Any]]:
        return [t.to_openai_schema() for t in self.list_tools(category)]

    def categories(self) -> List[str]:
        return sorted({t.category for t in self._tools.values()})

    # -- Execution --

    def execute(self, name: str, arguments: str | Dict[str, Any]) -> str:
        """Execute a tool by name, return result as JSON string."""
        desc = self._tools.get(name)
        if not desc:
            return json.dumps({"error": f"Unknown tool: {name}"}, ensure_ascii=False)

        if isinstance(arguments, str):
            try:
                args = json.loads(arguments) if arguments.strip() else {}
            except json.JSONDecodeError as exc:
                return json.dumps({"error": f"Invalid JSON arguments: {exc}"}, ensure_ascii=False)
        else:
            args = arguments

        try:
            result = desc.handler(**args) if isinstance(args, dict) else desc.handler(args)
            if isinstance(result, str):
                return result
            return json.dumps(result, ensure_ascii=False, default=str)
        except Exception:
            return json.dumps({
                "error": traceback.format_exc(limit=3),
            }, ensure_ascii=False)

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
