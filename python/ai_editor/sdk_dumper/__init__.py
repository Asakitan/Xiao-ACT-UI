"""SDK Dumper — memory-based game engine SDK extraction.

Reads live process memory via Engine A (mem_probe.rt_io) and extracts
class/struct definitions, field offsets, method signatures for:
  - Unity IL2CPP
  - Unity Mono
  - Unreal Engine 4/5
  - Source Engine (Valve)

Usage:
    from ai_editor.sdk_dumper import create_dumper, detect_engine
    engine = detect_engine(pid)
    dumper = create_dumper(engine, pid)
    sdk = dumper.dump()
"""

from ai_editor.sdk_dumper.base import (
    MemoryReader, ProcessReader, SDKDumper, SDKResult,
    SDKClass, SDKField, SDKMethod,
    detect_engine, create_dumper, list_engines,
)

__all__ = [
    "MemoryReader", "ProcessReader", "SDKDumper", "SDKResult",
    "SDKClass", "SDKField", "SDKMethod",
    "detect_engine", "create_dumper", "list_engines",
]
