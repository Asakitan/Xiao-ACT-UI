namespace SaoAuto.Core.Packets;

/// <summary>
/// Stamina-related decoding helpers ported from <c>_sao_cy_packet.pyx</c>:
/// <list type="bullet">
///   <item><c>is_sane_attr_stamina_max</c></item>
///   <item><c>decode_dirty_energy_value</c></item>
///   <item><c>normalize_season_medal_level</c></item>
/// </list>
/// These live in <c>SaoAuto.Core/Packets</c> rather than <c>State</c> because they
/// are part of the on-the-wire decode step — they reject malformed dirty-stream
/// values before they reach <see cref="Core.State.GameStateManager"/>.
/// </summary>
public static class StaminaDecoder
{
    /// <summary>STA cap acceptance heuristic: <c>0 &lt; value &lt;= 1300</c>.</summary>
    public static bool IsSaneAttrStaminaMax(long value) => value > 0 && value <= 1300;

    /// <summary>
    /// Pick the sane stamina value out of a dirty-stream pair (u32 + f32). Prefers
    /// the f32 when finite and within the dynamic cap; falls back to the u32 when
    /// it is non-negative and within cap. Returns <c>null</c> on rejection.
    /// </summary>
    /// <param name="rawU32">u32 sample from the dirty payload.</param>
    /// <param name="rawF32">f32 sample from the dirty payload.</param>
    /// <param name="staminaMax">Current cap (0 = unknown). Drives <c>max_allowed</c>.</param>
    public static double? DecodeDirtyEnergyValue(uint rawU32, float rawF32, long staminaMax = 0)
    {
        var maxAllowed = staminaMax > 0
            ? Math.Max(staminaMax * 1.2, 20000.0)
            : 20000.0;

        var finite = !float.IsNaN(rawF32) && !float.IsInfinity(rawF32);
        if (finite)
        {
            if (rawF32 >= 0.0f && rawF32 <= 1.05f && staminaMax > 0)
            {
                return rawF32;
            }
            if (rawF32 >= 0.01f && rawF32 <= maxAllowed)
            {
                return rawF32;
            }
            if (rawF32 == 0.0f)
            {
                return 0.0;
            }
        }
        if (rawU32 <= maxAllowed)
        {
            return rawU32;
        }
        return null;
    }

    /// <summary>Clamp a season-medal level to <c>&gt;= 0</c>.</summary>
    public static int NormalizeSeasonMedalLevel(long rawLevel) =>
        rawLevel < 0 ? 0 : (int)rawLevel;
}
