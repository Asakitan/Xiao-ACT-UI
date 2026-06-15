# Route-2: pure-memory `id -> name` from IL2CPP config tables

**Goal.** Mint *new* `id -> name` rows for the runtime name tables straight from the
game's own memory, with **no neighbour repo** (StarResonanceDps / resonance-logs).

## Why route-1 alone is not enough

`live_string_pool_all_localization.json` is the full `LocalizationStringPool`
(`allLocalizationString`, ≈108k rows) as **`string_index -> text`**. It contains
*every* localized string (skill/buff names, but also dialogue, tips, formulae).
It carries **no game id and no kind**. The only `id <-> text` bridges we have
today (`live_probe_act_matched_rows.json`, `target_matches`) obtained their ids
by string-matching against the neighbour `BuffTable`/`BuffName` — which is gone
on this machine, and only ever covered ~112 ids (all already in our tables).

To place memory names into `id -> name` tables we need an **id-keyed bridge**:
for each game id, which `allLocalizationString` index is its display name. That
mapping lives in the IL2CPP **config tables** (e.g. `SkillTable`, `BuffTable`,
`MonsterTable`), each entry holding an id and a name **string index** (the same
index space as `allLocalizationString`).

## Deliverable (C# mem probe extension)

Produce `assets/name_tables/live_id_name_index.json`:

```jsonc
{
  "skill_id":   { "<id>": <string_index>, ... },
  "buff_id":    { "<id>": <string_index>, ... },
  "monster_id": { "<id>": <string_index>, ... },
  "dungeon_id": { "<id>": <string_index>, ... }
}
```

`<string_index>` indexes the same array dumped in
`live_string_pool_all_localization.json` (`rows[i].index == i`). Nothing else is
required — the Python side resolves the text and classifies the kind.

## How to read the tables (IL2CPP, read-only `PROCESS_VM_READ`)

The string-pool anchor already proves the walking technique
(`live_string_pool_all_localization.json -> anchor`):

```
ZUtil.ZSingleton<StringPoolManager>.static_fields.instance_
  -> StringPoolManager.impl_
  -> StringPoolRuntimeImpl.allLocalizationString_   (System.String[])
```

Each config table is reachable the same way — a `ZSingleton<XxxTableManager>` (or
a `Dictionary<int, XxxConfig>` field on it). For each table:

1. Resolve the singleton instance via its `TypeInfo` static-fields (same chain
   shape as `StringPoolManager`; `verified_runtime_classes` already lists the
   pattern). Class names to look for: `SkillTable`/`SkillConfig`,
   `BuffTable`/`BuffConfig`, `MonsterTable`/`MonsterConfig` (confirm via the
   IL2CPP metadata class list the probe already enumerates).
2. Walk the backing `Dictionary<int, T>` (IL2CPP layout: `entries` array of
   `{ hashCode:i32, next:i32, key:i32, value:ptr }`, `count` at a fixed offset)
   **or** a `T[]` array (`+0x10 length`, `+0x18 element0`).
3. For each entry/element `T` (a `*Config` object), read:
   - the **id** field (`key` for a Dictionary, or `Config.Id` i32);
   - the **name string index** field — the i32 the game later feeds to
     `StringPoolManager.Get(index)` to render the display name (commonly named
     `Name`, `NameId`, `NameStringId`, `NameKey`). Validate by cross-checking a
     few known ids against `allLocalizationString[index]`.
4. Emit `{id_space: {id: name_index}}`.

Reuse the existing AVX2 scanner (`mem_probe.cy_memscan`,
`find_aligned_u64_in_set` / `scan_repeated_field_candidates`) to locate the
table arrays in candidate regions — it is now ~8x faster for the pointer-set
sweep (see `perf(mem_probe)` commit).

`id_space` values must match `net.tcp_name_cache._LIVE_ID_SPACE_KIND`
(`skill_id`, `buff_id`, `monster_id`, `dungeon_id`, `scene_id`, `npc_id`,
`item_id`, `sub_profession_skill_id`).

## Python consumption (already implemented, self-contained)

```bash
# dry-run: classify + report, write nothing
python -m tools.tablekit.mem_name_ingest --idmap assets/name_tables/live_id_name_index.json --dry-run
# apply: overlay memory names onto the on-disk <kind>.json tables
python -m tools.tablekit.mem_name_ingest --idmap assets/name_tables/live_id_name_index.json --apply
```

`mem_name_ingest.py`:
1. loads the string pool (`index -> text`) and the id-name-index bridge;
2. builds rows and funnels them through the **same** compact builders the
   runtime uses (`build_index_from_live_rows` + `sanitize_shared_cache`);
3. classifies every `(id_space, id)` via the **self-contained classifier**
   (`classify_id` -> committed tables + cache + name regex; no neighbour);
4. folds the result onto the tables with
   `hybrid_name_tables.overlay_cache_into_existing_tables` (cache wins, no
   classifier rerun, no wipe).

Validated end-to-end with a synthetic `idmap` (string-pool indices 13297/13301/
13318): `buff_id 2206180 -> player_buff "神圣壁垒"`, `skill_id 2414 ->
system_skill "神圣壁垒"` — same memory text, correctly split by id domain.

## Status

- Route-1 (self-contained classifier + full rebuild): **done** (commit `d41c526`).
- Python ingestion pipeline (`mem_name_ingest.py`): **done**, both bridges proven.
- Route-2 C# probe (`live_id_name_index.json`): **specified here**, needs the game
  running + the C# probe change. Once it emits the file, the Python side ingests
  it with the command above — no further Python work required.
