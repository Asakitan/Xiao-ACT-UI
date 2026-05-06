using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// AutoKey config + condition evaluation skeleton ported from
/// <c>auto_key_engine.py</c>. The actual `SendInput` keystroke layer lands
/// in Session 11b; here we expose the testable pieces: profile model,
/// trigger conditions, scheduling math.
/// </summary>
public sealed record AutoKeyProfile(
    string Name,
    ImmutableArray<AutoKeyAction> Actions,
    bool Enabled = true);

public sealed record AutoKeyAction(
    string Id,
    AutoKeyTrigger Trigger,
    KeyStroke KeyStroke,
    int CooldownMs,
    int Priority);

public sealed record KeyStroke(int VirtualKey, AutoKeyModifiers Modifiers, int HoldMs = 0)
{
    public override string ToString() =>
        $"{(Modifiers != AutoKeyModifiers.None ? Modifiers + "+" : "")}VK_0x{VirtualKey:X2}";
}

[Flags]
public enum AutoKeyModifiers
{
    None = 0,
    Shift = 1,
    Ctrl = 2,
    Alt = 4,
}

public abstract record AutoKeyTrigger;

public sealed record HpBelowTrigger(double Threshold) : AutoKeyTrigger;
public sealed record StaminaBelowTrigger(double Threshold) : AutoKeyTrigger;
public sealed record BurstReadyTrigger() : AutoKeyTrigger;
public sealed record BossPhaseTrigger(int Phase) : AutoKeyTrigger;

/// <summary>Pure trigger evaluator — called by the engine with the current snapshot.</summary>
public static class AutoKeyTriggers
{
    public static bool ShouldFire(AutoKeyTrigger trigger, AutoKeyContext ctx) => trigger switch
    {
        HpBelowTrigger hp => ctx.HpPct <= hp.Threshold,
        StaminaBelowTrigger sta => ctx.StaminaPct <= sta.Threshold,
        BurstReadyTrigger => ctx.BurstReady,
        BossPhaseTrigger bp => ctx.BossPhase == bp.Phase,
        _ => false,
    };
}

public readonly record struct AutoKeyContext(
    double HpPct,
    double StaminaPct,
    bool BurstReady,
    int BossPhase,
    bool InCombat,
    DateTimeOffset Now);

/// <summary>Cooldown gate that records last-fire timestamps per action id.</summary>
public sealed class AutoKeyCooldownGate
{
    private readonly Dictionary<string, DateTimeOffset> _lastFire = new();
    private readonly object _gate = new();

    public bool TryFire(string actionId, int cooldownMs, DateTimeOffset now)
    {
        lock (_gate)
        {
            if (_lastFire.TryGetValue(actionId, out var last))
            {
                if ((now - last).TotalMilliseconds < cooldownMs) return false;
            }
            _lastFire[actionId] = now;
            return true;
        }
    }

    public void Reset()
    {
        lock (_gate) _lastFire.Clear();
    }
}
