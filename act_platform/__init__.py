# -*- coding: utf-8 -*-
"""ACT platform primitives shared by WebView, Entity, replay, and plugins."""

from .event_bus import EventBus
from .events import ACT_EVENT_TOPICS, ACT_EVENT_SCHEMA_VERSION, make_event
from .plugins import PluginContext, PluginManager
from .adapters import (
    ParserAdapter,
    ParserAdapterMetadata,
    StarResonanceParserAdapter,
    built_in_parser_adapters,
    create_builtin_parser_adapter,
)

__all__ = [
    "ACT_EVENT_SCHEMA_VERSION",
    "ACT_EVENT_TOPICS",
    "EventBus",
    "ParserAdapter",
    "ParserAdapterMetadata",
    "PluginContext",
    "PluginManager",
    "StarResonanceParserAdapter",
    "built_in_parser_adapters",
    "create_builtin_parser_adapter",
    "make_event",
]
