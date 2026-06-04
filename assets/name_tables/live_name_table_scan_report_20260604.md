# Live Name Table Scan Report — 2026-06-04

## Scope

- Process: `Star.exe`, PID `20500`.
- Goal: scan the current live localization/name table and produce a name + TCP/config correspondence artifact.
- Historical full table data was used as a text reference only. Historical heap addresses are stale.

## Live StringPool Result

- `System.String` klass: `0x30252490`.
- Inferred `allLocalizationString_` array:
  - `array_obj`: `0x4479C5000`
  - `element_base`: `0x4479C5020`
  - `length`: `108090`
- Exported full table: `assets/name_tables/live_string_pool_all_localization.json`.
- Anchor status: `text_aligned_pointer_table_fallback`.
- Static singleton status: unresolved for this session because the available `script.json`/TypeInfo chain did not validate against the current process.
- Text alignment proof:
  - pointer table `0` matched the reference full table slice exactly.
  - global start index: `12463`.
  - matched slice rows: `1127/1127`.
- Full decode summary:
  - rows: `108090`
  - non-empty texts: `108046`
  - reference text mismatches: `0`
  - decode status: `ok=108047`, `length_out_of_range=43`.

## TCP / Config Correspondence

- Runtime pointer-table matched rows: `97`.
- Anchored matched rows: `96`.
- Unique matched texts: `96`.
- Matched primary ID spaces:
  - `buff_id`: `89`
  - `skill_id`: `5`
  - `skill_id_or_legacy_skill_name_index`: `1`
  - `sub_profession_skill_id`: `2`
- Compact preparse cache: `assets/name_tables/tcp_preparse_name_cache.json`.
  - `buff`: `89`
  - `skill`: `7`
  - `player`: `1` from short live PacketBridge sampling.
- Final composed table: `assets/name_tables/live_name_tcp_correspondence.json`.
  - total entries: `108090`
  - TCP/config matched entries: `96`
  - cache endpoint count: `1`

## Live TCP Endpoint Sample

- Short passive PacketBridge sample duration: ~30 seconds.
- Server endpoint observed:
  - endpoint hex: `d239471c:2131`
  - IP: `210.57.71.28`
  - port: `2131`
  - seen count: `396`
- The short sample observed one player UID entry (`36668136`) and one self-buff publish, but name/level remained empty because the parser warns those fields are only emitted on login or map switch.

## Limitations

- This scan did not claim a static singleton anchor. The current full table is a validated text-aligned fallback export.
- `live_name_tcp_correspondence.json` contains all localization names, but only entries with known TCP/config/community IDs are marked as matched.
- Endpoint annotations are present in the cache endpoint section and player entry. Static buff/skill matches do not automatically inherit the live endpoint unless observed in decoded TCP events.
- Runtime heap addresses are valid only for this live session.
