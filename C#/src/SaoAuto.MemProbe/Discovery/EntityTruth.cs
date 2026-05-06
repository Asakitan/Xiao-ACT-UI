namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// One ground-truth observation harvested from the packet bridge: a monster
/// (or self) with current HP and max HP. Used by
/// <see cref="AnchorDiscoveryEngine"/> to drive value-anchor scans against
/// the target process. Mirrors <c>mem_probe.entity_discovery.MonsterTruth</c>.
/// </summary>
public readonly record struct EntityTruth(
    ulong Uuid,
    long Hp,
    long MaxHp,
    string Name);

/// <summary>
/// Result of a single discovery pass — the set of memory addresses that
/// matched the supplied truths.
/// </summary>
public sealed record DiscoveryResult(
    int TruthsConsidered,
    IReadOnlyList<EntityHit> Hits)
{
    public bool IsConfident => Hits.Count > 0 && TruthsConsidered >= 2;
}

/// <summary>
/// One candidate entity address — the offset where the uuid was found, plus
/// the address that satisfied the (uuid, hp, maxHp) triple.
/// </summary>
public readonly record struct EntityHit(
    ulong RegionBase,
    int UuidOffset,
    ulong Uuid,
    long ObservedHp,
    long ObservedMaxHp);
