import json

# Check SkillTable.json structure
d = json.load(open(r'E:\VC\SAO-UI\StarResonanceDps\DataTools\Data\CN\SkillTable.json', 'r', encoding='utf-8'))

# Build a simplified skill name map {id: name}
skill_map = {}
for k, v in d.items():
    name = v.get('Name', '')
    sid = int(k)
    if name and sid > 0:
        skill_map[sid] = name

print(f"Total skills in SkillTable.json: {len(skill_map)}")

# Check 神盾骑士 skills (prefix 24)
print("\n神盾骑士 skills (24xx):")
for sid in sorted(skill_map):
    s = str(sid)
    if s.startswith('24') and len(s) == 4:
        print(f"  {sid}: {skill_map[sid]}")

# Current skill_names.json
cur = json.load(open('assets/skill_names.json', 'r', encoding='utf-8'))
cur_ids = set(int(k) for k in cur if str(k).isdigit())
print(f"\nCurrent skill_names.json: {len(cur_ids)} entries")
print(f"SkillTable.json: {len(skill_map)} entries")
print(f"Missing from current: {len(set(skill_map.keys()) - cur_ids)} entries")

# Show some missing skills that are in common ranges
missing = sorted(skill_map.keys() - cur_ids)
print(f"\nExample missing skills:")
for sid in missing[:20]:
    print(f"  {sid}: {skill_map[sid]}")

# Also check skill_names_simplified (458 entries)
simp = json.load(open(r'E:\VC\SAO-UI\StarResonanceDps\DataTools\Old\Data\skill\skill_names_simplified.json', 'r', encoding='utf-8'))
print(f"\nskill_names_simplified.json: {len(simp)} entries")
simp_ids = set(int(k) for k in simp if str(k).isdigit())
missing_simp = sorted(simp_ids - cur_ids)
print(f"Extra in simplified vs current: {len(missing_simp)}")
for sid in missing_simp[:20]:
    print(f"  {sid}: {simp[str(sid)]}")
