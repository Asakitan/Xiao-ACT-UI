namespace SaoAuto.MemProbe.Watchers;

/// <summary>
/// SELF object payload emitted by <see cref="MemSelfWatcher"/>. Mirrors
/// the relevant fields of Python <c>packet_parser.PlayerData.to_dict()</c>
/// so consumers (state manager, DPS tracker, web bridge) cannot tell mem
/// from TCP. Fields not yet readable from memory (skill maps, names) are
/// left at their defaults.
/// </summary>
public sealed record SelfSnapshot
{
    public ulong Uid { get; init; }
    public ulong Uuid { get; init; }
    public string Name { get; init; } = string.Empty;
    public int Level { get; init; }
    public int LevelExtra { get; init; }
    public long SeasonExp { get; init; }
    public long Hp { get; init; }
    public long MaxHp { get; init; }
    public int ProfessionId { get; init; }
    public string Profession { get; init; } = string.Empty;
    public long FightPoint { get; init; }
    public int StaminaCurrent { get; init; }
    public int StaminaMax { get; init; }
    public int EnergyCurrent { get; init; }
    public int EnergyTotal { get; init; }
    public string Source { get; init; } = "mem";
    public double Timestamp { get; init; }
}

/// <summary>
/// Offsets needed to read the SELF object end-to-end without an IL2CPP
/// dump. All offsets are in BYTES from the corresponding object base.
/// Mirrors Python <c>SelfReadConfig</c>.
/// </summary>
public sealed class SelfReadConfig
{
    public ulong CharObj { get; init; }
    public int UidOff { get; init; } = -1;
    public int AttrSlotOff { get; init; } = -1;

    public int CharBaseSlotOff { get; init; } = -1;
    public int RoleLevelSlotOff { get; init; } = -1;
    public int ProfessionListSlotOff { get; init; } = -1;
    public int EnergyItemSlotOff { get; init; } = -1;
    public int SeasonMedalSlotOff { get; init; } = -1;

    public ulong AttrObj { get; init; }
    public int CurHpOff { get; init; } = -1;
    public int MaxHpOff { get; init; } = -1;
    public int HpWidth { get; init; } = 8;

    public int RoleLevelFieldOff { get; init; } = -1;
    public int ProfessionFieldOff { get; init; } = -1;
    public int EnergyFieldOff { get; init; } = -1;
    public int FightPointFieldOff { get; init; } = -1;

    public ulong CharId { get; init; }
}
