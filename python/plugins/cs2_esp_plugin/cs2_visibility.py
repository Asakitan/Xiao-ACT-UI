# -*- coding: utf-8 -*-
"""CS2 plugin — visibility check via spotted state.

CS2 tracks whether entities are "spotted" by each player using
m_entitySpottedState.m_bSpottedByMask (a per-player bitmask) and
m_bSpotted (a boolean).

This is NOT a true line-of-sight raycast — it relies on the game's
own spotted state, which is updated when a player has a line of sight
to an enemy (smoke grenades block it).  For most purposes this is
sufficient and much cheaper than BSP-based ray tracing.
"""

from __future__ import annotations

from typing import Optional, Sequence


def is_visible_by_mask(spotted_mask: int, local_player_index: int) -> bool:
    """Check if the local player can see this entity via m_bSpottedByMask.

    ``local_player_index`` is the entity slot index of the local player
    (typically 1-based: bit = 1 << (index - 1), but some builds use
    0-based: bit = 1 << index).  We check both to be safe.
    """
    if spotted_mask == 0:
        return False
    if local_player_index <= 0:
        return False
    bit_1 = 1 << (local_player_index - 1)
    bit_0 = 1 << local_player_index
    return bool(spotted_mask & (bit_1 | bit_0))


def is_spotted(entity: dict) -> bool:
    """Check the simple m_bSpotted boolean on an entity."""
    return bool(entity.get("spotted", False))


def annotate_visibility(entities: Sequence[dict],
                        local_index: int,
                        use_mask: bool = True) -> list:
    """Add ``is_visible`` to each entity dict.

    Modifies in-place and returns the list for chaining.
    """
    out = []
    for ent in entities:
        e = dict(ent)
        if use_mask:
            mask = e.get("spotted_mask", 0)
            e["is_visible"] = is_visible_by_mask(mask, local_index)
        else:
            e["is_visible"] = is_spotted(e)
        out.append(e)
    return out
