namespace SaoAuto.Core.Automation;

/// <summary>
/// PROTO-02 — Port of Python's <c>_remember_seen_skill</c> +
/// <c>_try_detect_profession</c> at packet_parser.py:1832-1879. Caches the
/// set of skill ids observed for each player uuid and reverse-looks the
/// base skill id into a profession id when SyncContainerData hasn't yet
/// confirmed it. Static — no instance state — because the observation
/// belongs to the parser shell and pre-dates any per-encounter scope.
///
/// <para>The reverse-lookup tables mirror the canonical tables Python keeps
/// at packet_parser.py:654-743 (<c>PROFESSION_NORMAL_ATTACK</c>,
/// <c>PROFESSION_SKILL</c>, <c>PROFESSION_ULTIMATE</c>,
/// <c>PROFESSION_SKILL_VARIANTS</c>, <c>SUB_PROFESSION_NAMES</c>).</para>
/// </summary>
public static class SkillObserver
{
    /// <summary>Profession ids matched against
    /// <see cref="State.GameState.ProfessionId"/>. Mirrors Python's
    /// <c>PROFESSION_NAMES</c> at packet_parser.py:639.</summary>
    public static readonly IReadOnlyDictionary<int, string> ProfessionNames =
        new Dictionary<int, string>
        {
            [1] = "雷影剑士",
            [2] = "冰魔导师",
            [3] = "涤罪恶火·战斧",
            [4] = "青岚骑士",
            [5] = "森语者",
            [8] = "雷霆一闪·手炮",
            [9] = "巨刃守护者",
            [10] = "暗灵祈舞·仪刀",
            [11] = "神射手",
            [12] = "神盾骑士",
            [13] = "灵魂乐手",
        };

    /// <summary>Base-skill-id → profession-id reverse table populated from
    /// the three canonical tables (NORMAL_ATTACK / SKILL / ULTIMATE) plus
    /// the sub-profession SKILL_VARIANTS. Mirrors Python's
    /// <c>_SKILL_TO_PROFESSION</c> dict built at packet_parser.py:830-841.</summary>
    public static readonly IReadOnlyDictionary<int, int> SkillToProfession =
        BuildSkillToProfession();

    /// <summary>Base skill id → sub-profession branch name (e.g. 1714 → "居合").
    /// Mirrors Python's <c>SUB_PROFESSION_NAMES</c> at packet_parser.py:717.</summary>
    public static readonly IReadOnlyDictionary<int, string> SubProfessionNames =
        new Dictionary<int, string>
        {
            // 雷影剑士
            [1714] = "居合", [1734] = "居合",
            [44701] = "月刃", [179906] = "月刃",
            // 冰魔导师
            [120901] = "冰矛", [120902] = "冰矛", [1242] = "冰矛",
            [1241] = "射线",
            // 涤罪恶火
            [1605] = "无相", [1606] = "赤红",
            // 青岚骑士
            [1405] = "重装", [1418] = "重装",
            [1419] = "空枪",
            // 森语者
            [1518] = "惩戒", [1541] = "惩戒", [21402] = "惩戒",
            [20301] = "愈合",
            // 巨刃守护者
            [199902] = "岩盾",
            [1930] = "格挡", [1931] = "格挡", [1934] = "格挡", [1935] = "格挡", [1922] = "格挡",
            // 神射手
            [2292] = "狼弓", [1700820] = "狼弓", [1700825] = "狼弓", [1700827] = "狼弓",
            [220112] = "鹰弓", [2203622] = "鹰弓", [220106] = "鹰弓",
            // 神盾骑士
            [2405] = "防盾", [2406] = "光盾",
            // 灵魂乐手
            [2306] = "狂音",
            [2307] = "协奏", [2361] = "协奏", [55302] = "协奏",
        };

    private static IReadOnlyDictionary<int, int> BuildSkillToProfession()
    {
        var dict = new Dictionary<int, int>();
        // Profession-id → base-skill-id for NORMAL_ATTACK / SKILL / ULTIMATE.
        // Mirrors Python's three tables at packet_parser.py:654-696.
        var normalAttack = new Dictionary<int, int>
        {
            [1] = 1701, [2] = 1201, [3] = 1601, [4] = 1401, [5] = 1501,
            [8] = 1801, [9] = 1901, [10] = 2101, [11] = 2201, [12] = 2401,
            [13] = 2321,
        };
        var skill = new Dictionary<int, int>
        {
            [1] = 1714, [2] = 1242, [3] = 1609, [4] = 1418, [5] = 1518,
            [8] = 1806, [9] = 1922, [10] = 2105, [11] = 2220, [12] = 2405,
            [13] = 2306,
        };
        var ultimate = new Dictionary<int, int>
        {
            [1] = 1713, [2] = 1248, [3] = 1614, [4] = 1426, [5] = 1509,
            [8] = 1808, [9] = 1907, [10] = 2108, [11] = 2209, [12] = 2407,
            [13] = 2314,
        };
        foreach (var kv in normalAttack) dict[kv.Value] = kv.Key;
        foreach (var kv in skill) dict[kv.Value] = kv.Key;
        foreach (var kv in ultimate) dict[kv.Value] = kv.Key;
        // SKILL_VARIANTS: each profession has 1-3 alternative slot-2 skills.
        // Mirrors Python's <c>PROFESSION_SKILL_VARIANTS</c> at 701-713.
        var variants = new Dictionary<int, int[]>
        {
            [1] = new[] { 1714, 44701 },
            [2] = new[] { 1242, 1241 },
            [3] = new[] { 1609, 1605, 1606 },
            [4] = new[] { 1418, 1419 },
            [5] = new[] { 1518, 20301 },
            [8] = new[] { 1806 },
            [9] = new[] { 1922, 1930, 199902 },
            [10] = new[] { 2105 },
            [11] = new[] { 2220, 2292, 220112 },
            [12] = new[] { 2405, 2406 },
            [13] = new[] { 2306, 2307 },
        };
        foreach (var (pid, sids) in variants)
        {
            foreach (var sid in sids) dict[sid] = pid;
        }
        return dict;
    }

    /// <summary>Compute the base skill id from a skill_level_id. Mirrors
    /// Python's <c>base = skill_level_id // 100 if skill_level_id &gt;= 100
    /// else skill_level_id</c> at packet_parser.py:1840.</summary>
    public static int BaseFromSkillLevelId(int skillLevelId)
    {
        if (skillLevelId <= 0) return 0;
        return skillLevelId >= 100 ? skillLevelId / 100 : skillLevelId;
    }

    /// <summary>Reverse-lookup a profession id from any observed skill id.
    /// Returns 0 when the skill id is not on any profession's canonical list.
    /// Mirrors Python's <c>_SKILL_TO_PROFESSION.get(base, 0)</c> at line 1854.</summary>
    public static int DetectProfession(int skillLevelId)
    {
        var b = BaseFromSkillLevelId(skillLevelId);
        return SkillToProfession.TryGetValue(b, out var pid) ? pid : 0;
    }

    /// <summary>Reverse-lookup a sub-profession branch name. Returns empty
    /// when the skill id is not a recognised sub-prof variant. Mirrors
    /// Python's <c>SUB_PROFESSION_NAMES.get(base, '')</c> at line 1843.</summary>
    public static string DetectSubProfession(int skillLevelId)
    {
        var b = BaseFromSkillLevelId(skillLevelId);
        return SubProfessionNames.TryGetValue(b, out var name) ? name : string.Empty;
    }
}
