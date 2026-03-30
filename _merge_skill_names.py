"""Merge skill names from SkillTable.json (3249 entries) + skill_names_simplified
into a single skill_names.json for use by the packet bridge."""
import json, os

out = {}  # int_id → str_name

# 1. Load current skill_names.json (440 entries) as baseline
cur_path = os.path.join(os.path.dirname(__file__), 'assets', 'skill_names.json')
if os.path.exists(cur_path):
    with open(cur_path, 'r', encoding='utf-8') as f:
        cur = json.load(f)
    for k, v in cur.items():
        if str(k).isdigit() and v:
            out[int(k)] = str(v)
    print(f"Loaded {len(out)} from current skill_names.json")

# 2. Load SkillTable.json (CN) — most complete source  
st_path = os.path.join(os.path.dirname(__file__), '..', 'StarResonanceDps',
                        'DataTools', 'Data', 'CN', 'SkillTable.json')
if os.path.exists(st_path):
    with open(st_path, 'r', encoding='utf-8') as f:
        st = json.load(f)
    added = 0
    for k, v in st.items():
        sid = int(k)
        name = v.get('Name', '')
        if sid > 0 and name and sid not in out:
            # Skip generic placeholder names like "普通攻击01"
            if '01' in name and '普通' in name:
                continue
            out[sid] = name
            added += 1
    print(f"Added {added} from SkillTable.json (total now {len(out)})")

# 3. Load skill_names_simplified (458 entries)  
simp_path = os.path.join(os.path.dirname(__file__), '..', 'StarResonanceDps',
                          'DataTools', 'Old', 'Data', 'skill', 'skill_names_simplified.json')
if os.path.exists(simp_path):
    with open(simp_path, 'r', encoding='utf-8') as f:
        simp = json.load(f)
    added = 0
    for k, v in simp.items():
        if str(k).isdigit() and v:
            sid = int(k)
            if sid > 0 and sid not in out:
                out[sid] = str(v)
                added += 1
    print(f"Added {added} from skill_names_simplified.json (total now {len(out)})")

# Write merged output
with open(cur_path, 'w', encoding='utf-8') as f:
    json.dump({str(k): v for k, v in sorted(out.items())}, f, ensure_ascii=False, indent=1)
print(f"\nWrote {len(out)} entries to {cur_path}")

# Show 神盾骑士 skills as verification
print("\n神盾骑士 skills (24xx):")
for sid in sorted(out):
    s = str(sid)
    if s.startswith('24') and len(s) == 4:
        print(f"  {sid}: {out[sid]}")
