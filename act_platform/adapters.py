# -*- coding: utf-8 -*-
"""ACT parser adapter contracts and built-in parser wrappers."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Mapping, Optional

from packet_parser import PacketParser

from .events import make_event


def _str_tuple(value: Any, default: tuple[str, ...] = ()) -> tuple[str, ...]:
    if value is None:
        return tuple(default)
    if isinstance(value, str):
        text = value.strip()
        return (text,) if text else tuple(default)
    if isinstance(value, (list, tuple, set)):
        out = tuple(str(item).strip() for item in value if str(item or "").strip())
        return out or tuple(default)
    return tuple(default)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


@dataclass(frozen=True)
class ParserAdapterMetadata:
    """Serializable description of a parser/game adapter."""

    adapter_id: str
    game_id: str
    display_name: str
    supported_locales: tuple[str, ...] = field(default_factory=lambda: ("zh-CN",))
    source_kinds: tuple[str, ...] = field(default_factory=lambda: ("packet",))
    priority: float = 0.0
    version: str = "1"
    description: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": str(self.adapter_id or ""),
            "adapter_id": str(self.adapter_id or ""),
            "game_id": str(self.game_id or ""),
            "display_name": str(self.display_name or ""),
            "supported_locales": list(self.supported_locales),
            "source_kinds": list(self.source_kinds),
            "priority": float(self.priority or 0.0),
            "version": str(self.version or ""),
            "description": str(self.description or ""),
        }

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "ParserAdapterMetadata":
        adapter_id = str(value.get("adapter_id") or value.get("id") or "").strip()
        display_name = str(value.get("display_name") or value.get("title") or "").strip() or adapter_id
        return cls(
            adapter_id=adapter_id,
            game_id=str(value.get("game_id") or "").strip(),
            display_name=display_name,
            supported_locales=_str_tuple(value.get("supported_locales") or value.get("locales"), ("zh-CN",)),
            source_kinds=_str_tuple(value.get("source_kinds"), ("packet",)),
            priority=_safe_float(value.get("priority"), 0.0),
            version=str(value.get("version") or "1"),
            description=str(value.get("description") or ""),
        )


class ParserAdapter:
    """Small runtime contract for parser/game integrations."""

    def __init__(self, metadata: ParserAdapterMetadata) -> None:
        self.metadata = metadata
        self.started = False

    @property
    def adapter_id(self) -> str:
        return self.metadata.adapter_id

    @property
    def game_id(self) -> str:
        return self.metadata.game_id

    def start(self) -> "ParserAdapter":
        self.started = True
        return self

    def stop(self) -> None:
        self.started = False

    def parse_packet(self, frame: bytes) -> Any:
        raise NotImplementedError("parse_packet is not implemented by this adapter")

    def parse_log_line(self, line: str) -> list[dict[str, Any]]:
        return []

    def import_file(self, path: str) -> dict[str, Any]:
        return {
            "ok": False,
            "adapter_id": self.adapter_id,
            "reason": "import_file_not_supported",
            "path": str(path or ""),
        }

    def normalize_event(
        self,
        topic: str,
        payload: Optional[Mapping[str, Any]] = None,
        *,
        source_kind: str = "packet",
        confidence: float = 1.0,
        observed_at: Optional[float] = None,
    ) -> dict[str, Any]:
        return make_event(
            topic,
            payload,
            source_name=self.adapter_id,
            source_kind=source_kind,
            game_id=self.game_id,
            parser_id=self.adapter_id,
            confidence=confidence,
            observed_at=observed_at,
        )

    def health(self) -> dict[str, Any]:
        return {
            "adapter_id": self.adapter_id,
            "game_id": self.game_id,
            "started": bool(self.started),
            "metadata": self.metadata.to_dict(),
        }


STAR_RESONANCE_PARSER_METADATA = ParserAdapterMetadata(
    adapter_id="star_resonance_tcp",
    game_id="star_resonance",
    display_name="Star Resonance TCP parser",
    supported_locales=("zh-CN",),
    source_kinds=("packet",),
    priority=100.0,
    version="1",
    description="Built-in wrapper around packet_parser.PacketParser.",
)


class StarResonanceParserAdapter(ParserAdapter):
    """Built-in adapter that preserves the existing PacketParser behavior."""

    def __init__(self, metadata: ParserAdapterMetadata = STAR_RESONANCE_PARSER_METADATA) -> None:
        super().__init__(metadata)
        self.parser: Optional[PacketParser] = None
        self.packet_frames = 0

    def create_parser(
        self,
        *,
        on_self_update: Callable[..., Any],
        preferred_uid: int = 0,
        on_damage: Optional[Callable[[dict], Any]] = None,
        on_monster_update: Optional[Callable[[dict], Any]] = None,
        on_boss_event: Optional[Callable[[dict], Any]] = None,
        on_scene_change: Optional[Callable[..., Any]] = None,
        on_skill_event: Optional[Callable[[dict], Any]] = None,
        on_dungeon_event: Optional[Callable[[dict], Any]] = None,
    ) -> PacketParser:
        self.parser = PacketParser(
            on_self_update=on_self_update,
            preferred_uid=preferred_uid,
            on_damage=on_damage,
            on_monster_update=on_monster_update,
            on_boss_event=on_boss_event,
            on_scene_change=on_scene_change,
            on_skill_event=on_skill_event,
            on_dungeon_event=on_dungeon_event,
        )
        return self.parser

    def parse_packet(self, frame: bytes) -> Any:
        if self.parser is None:
            raise RuntimeError("parser is not created")
        self.packet_frames += 1
        return self.parser.process_packet(frame)

    process_packet = parse_packet

    def set_subscribed_messages(self, names: Optional[set]) -> None:
        if self.parser is not None and hasattr(self.parser, "set_subscribed_messages"):
            self.parser.set_subscribed_messages(names)

    def get_alive_monsters(self) -> list:
        return self.parser.get_alive_monsters() if self.parser is not None else []

    def get_players(self) -> dict:
        return self.parser.get_players() if self.parser is not None else {}

    def health(self) -> dict[str, Any]:
        out = super().health()
        parser_stats = dict(getattr(self.parser, "stats", {}) or {}) if self.parser is not None else {}
        out.update({
            "parser_created": self.parser is not None,
            "packet_frames": int(self.packet_frames),
            "parser_stats": parser_stats,
        })
        return out


def built_in_parser_adapters() -> list[dict[str, Any]]:
    return [STAR_RESONANCE_PARSER_METADATA.to_dict()]


def create_builtin_parser_adapter(adapter_id: str = "star_resonance_tcp") -> ParserAdapter:
    if str(adapter_id or "") == STAR_RESONANCE_PARSER_METADATA.adapter_id:
        return StarResonanceParserAdapter()
    raise KeyError(f"unknown built-in parser adapter: {adapter_id}")


__all__ = [
    "ParserAdapter",
    "ParserAdapterMetadata",
    "STAR_RESONANCE_PARSER_METADATA",
    "StarResonanceParserAdapter",
    "built_in_parser_adapters",
    "create_builtin_parser_adapter",
]
