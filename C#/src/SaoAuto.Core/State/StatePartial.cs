namespace SaoAuto.Core.State;

/// <summary>
/// S91 — Typed partial mutation for <see cref="GameStateManager.ApplyPartial"/>.
/// Each nullable field is "set" when non-null, "unchanged" when null. Mirrors
/// Python <c>game_state.GameStateManager.update(**kwargs)</c>'s
/// "is this key in the kwargs dict?" semantics for the HP/Level/Stamina
/// fields that participate in the rollback + cap rules. Other fields
/// (boss, identity, capture meta) keep flowing through the existing
/// <see cref="GameStateManager.Update"/> lambda API.
/// </summary>
public sealed record StatePartial
{
    public int? HpCurrent { get; init; }
    public int? HpMax { get; init; }
    public double? HpPct { get; init; }

    public int? StaminaCurrent { get; init; }
    public int? StaminaMax { get; init; }
    public double? StaminaPct { get; init; }

    public int? LevelBase { get; init; }
    public int? LevelExtra { get; init; }

    public bool IsEmpty =>
        HpCurrent is null && HpMax is null && HpPct is null
        && StaminaCurrent is null && StaminaMax is null && StaminaPct is null
        && LevelBase is null && LevelExtra is null;
}
