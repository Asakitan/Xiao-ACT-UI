using BenchmarkDotNet.Attributes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Packets;

namespace SaoAuto.Benchmarks;

/// <summary>
/// Combat / damage-pipeline microbenchmarks. Covers the
/// <c>compute_damage_key</c> and <c>DpsTracker.Snapshot</c> hot paths.
/// </summary>
[MemoryDiagnoser]
public class CombatBenchmarks
{
    [Params(64, 1024)]
    public int Events;

    [Benchmark]
    public long DamageKey_Sweep()
    {
        long acc = 0;
        for (var i = 0; i < Events; i++)
        {
            acc ^= CyPacketExtras.ComputeDamageKey(
                ownerId: 100 + i,
                damageSource: 700 + (i & 0x3F),
                ownerLevel: 60,
                hitEventId: i * 7L);
        }
        return acc;
    }

    [Benchmark]
    public DpsSnapshot DpsTracker_FillAndSnapshot()
    {
        var dps = new DpsTracker();
        for (var i = 0; i < Events; i++)
        {
            dps.RecordDamage(
                entityUuid: 1000 + (i & 0x07),
                entityName: "mob",
                amount: 1000 + i,
                professionId: 1,
                isSelf: (i & 1) == 0);
        }
        return dps.Snapshot();
    }
}
