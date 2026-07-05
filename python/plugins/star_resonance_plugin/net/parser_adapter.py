# -*- coding: utf-8 -*-
# Star Resonance ACT parser adapter owned by the plugin.

from __future__ import annotations

import os
import sys
from typing import Any, Callable, Mapping, Optional

from act_platform.adapters import ParserAdapter, ParserAdapterMetadata


_PLUGIN_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_PROTOCOL_ROOT = os.path.join(_PLUGIN_ROOT, "protocol")
if _PROTOCOL_ROOT not in sys.path:
    sys.path.insert(0, _PROTOCOL_ROOT)

from packet_parser import PacketParser  # type: ignore  # noqa: E402


def _noop(*_args: Any, **_kwargs: Any) -> None:
    return None


class StarResonanceParserAdapter(ParserAdapter):
    # Plugin-local wrapper around the Star Resonance TCP PacketParser.

    def __init__(self) -> None:
        super().__init__(ParserAdapterMetadata(
            adapter_id="star_resonance_tcp",
            game_id="star_resonance",
            display_name="Star Resonance TCP",
            supported_locales=("zh-CN",),
            source_kinds=("packet",),
            priority=100.0,
            version="1",
            description="Star Resonance TCP packet parser",
        ))
        self.parser: Optional[PacketParser] = None
        self.packet_frames = 0
        self._subscribed_messages: Optional[set[str]] = None

    def create_parser(
        self,
        *,
        on_self_update: Optional[Callable[[Any], None]] = None,
        preferred_uid: int = 0,
        on_damage: Optional[Callable[[dict], None]] = None,
        on_monster_update: Optional[Callable[[dict], None]] = None,
        on_boss_event: Optional[Callable[[dict], None]] = None,
        on_scene_change: Optional[Callable[..., None]] = None,
        on_skill_event: Optional[Callable[[dict], None]] = None,
        on_dungeon_event: Optional[Callable[[dict], None]] = None,
    ) -> PacketParser:
        self.parser = PacketParser(
            on_self_update=on_self_update or _noop,
            preferred_uid=int(preferred_uid or 0),
            on_damage=on_damage,
            on_monster_update=on_monster_update,
            on_boss_event=on_boss_event,
            on_scene_change=on_scene_change,
            on_skill_event=on_skill_event,
            on_dungeon_event=on_dungeon_event,
        )
        if self._subscribed_messages is not None:
            self.parser.set_subscribed_messages(set(self._subscribed_messages))
        return self.parser

    def parse_packet(self, frame: bytes) -> Any:
        self.packet_frames += 1
        if self.parser is None:
            self.create_parser()
        return self.parser.process_packet(bytes(frame or b"")) if self.parser is not None else None

    process_packet = parse_packet

    def set_subscribed_messages(self, names: Optional[set[str]]) -> None:
        self._subscribed_messages = None if names is None else {str(item) for item in names}
        if self.parser is not None:
            self.parser.set_subscribed_messages(
                None if self._subscribed_messages is None else set(self._subscribed_messages)
            )

    def get_alive_monsters(self) -> list:
        if self.parser is None:
            return []
        return list(self.parser.get_alive_monsters())

    def get_players(self) -> Mapping[int, Any]:
        if self.parser is None:
            return {}
        return self.parser.get_players()

    def health(self) -> dict[str, Any]:
        out = super().health()
        out.update({
            "parser_created": self.parser is not None,
            "packet_frames": int(self.packet_frames),
        })
        return out


__all__ = ["StarResonanceParserAdapter"]
