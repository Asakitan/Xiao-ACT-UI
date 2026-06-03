# -*- coding: utf-8 -*-
"""ACT platform primitives shared by WebView, Entity, replay, and plugins."""

from .event_bus import EventBus
from .events import ACT_EVENT_TOPICS, ACT_EVENT_SCHEMA_VERSION, make_event
from .plugins import EngineAccess, PluginContext, PluginManager
from .adapters import (
    ParserAdapter,
    ParserAdapterMetadata,
    PluginParserAdapter,
    StarResonanceParserAdapter,
    built_in_parser_adapters,
    create_builtin_parser_adapter,
    create_plugin_parser_adapter,
    plugin_parser_adapters,
)

__all__ = [
    "ACT_EVENT_SCHEMA_VERSION",
    "ACT_EVENT_TOPICS",
    "EventBus",
    "EngineAccess",
    "ParserAdapter",
    "ParserAdapterMetadata",
    "PluginContext",
    "PluginManager",
    "PluginParserAdapter",
    "StarResonanceParserAdapter",
    "built_in_parser_adapters",
    "create_builtin_parser_adapter",
    "create_plugin_parser_adapter",
    "plugin_parser_adapters",
    "make_event",
]
