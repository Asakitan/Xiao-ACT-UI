# Parity Fixture Generators

Python-side helpers that produce the JSON snapshots the C# parity test
suite (`tests/SaoAuto.ParityTests`) loads at runtime. Each generator
consumes a real input (a captured pcap, a PNG screenshot, a tape of
recorded events) and writes a fixture under
`tests/SaoAuto.ParityTests/Fixtures/`.

The fixtures are committed to the repo; the generators are run only
when the canonical Python runtime changes a contract that the C# port
has to keep matching.

## Common envelope

Every fixture is a JSON object:

```json
{
  "kind":   "<subsystem>",
  "source": "<input file path or sentinel>",
  "data":   { ...subsystem snapshot... }
}
```

`ParityFixture.Load(...)` in `tests/SaoAuto.ParityTests/ParityFixture.cs`
reads the whole object; tests assert on `data`. `JsonParity.FirstDifference`
walks subtrees with optional numeric tolerance.

## Generators

| Generator | Produces | Consumes |
| --- | --- | --- |
| `gen_packet_state.py`  | `packet_state.json`  | a captured pcap (Star.exe traffic) |
| `gen_recognition.py`   | `recognition_*.json` | a screenshot PNG of the game window |
| `gen_dps_snapshot.py`  | `dps_snapshot.json`  | recorded damage event tape (JSONL) |
| `gen_overlay_layout.py`| `overlay_layout.json`| no input — pure layout math sample |

## Workflow

```bash
# 1. Activate the Python venv that builds sao_auto (so the canonical
#    runtime is importable).
cd sao_auto && python -m venv .venv && .venv/Scripts/activate
pip install -e .

# 2. Run a generator from anywhere in the repo, pointing at the
#    SaoAuto.ParityTests fixtures dir.
python C#/tools/parity-fixtures/gen_dps_snapshot.py \
    --input  C#/tools/parity-fixtures/samples/dmg_tape.jsonl \
    --output C#/tests/SaoAuto.ParityTests/Fixtures/dps_snapshot.json

# 3. Re-run C# tests.
dotnet test C#/tests/SaoAuto.ParityTests
```

## Conventions

- All numeric fields are emitted as native JSON numbers; do not
  pre-stringify.
- Float fields that the C# side compares with `numericEpsilon` should
  be rounded to ≤6 decimal places to keep diffs readable.
- `source` is a human pointer (commit + offset, capture time, etc.) —
  it is not parsed by the loader.
- New generators should default to writing into
  `tests/SaoAuto.ParityTests/Fixtures/` and refuse to overwrite a
  fixture without `--force`.
