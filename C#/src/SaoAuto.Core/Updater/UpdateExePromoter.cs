namespace SaoAuto.Core.Updater;

/// <summary>
/// Pure decision helper ported from <c>config._promote_update_exe_new_early</c>
/// + <c>_promote_runtime_update_exe_early</c>. Tells the live filesystem
/// adapter (Session 12b) what to do without itself touching disk — keeps
/// the rules testable and gives a single source of truth for the safety
/// invariants Python documented in v2.1.2-h..k.
/// </summary>
public static class UpdateExePromoter
{
    /// <summary>
    /// Decide what to do with an `update.exe.new` staged file at the top level.
    /// </summary>
    public static UpdatePromotionAction DecideUpdateExeNew(UpdateExeContext ctx)
    {
        if (!ctx.IsMainAppHost) return UpdatePromotionAction.Skip;
        if (!ctx.HasUpdateExeNew) return UpdatePromotionAction.Skip;
        if (!ctx.HasUpdateExe) return UpdatePromotionAction.PromoteAtomicReplace;
        if (ctx.UpdateExeIdenticalToNew) return UpdatePromotionAction.DropStaged;
        return UpdatePromotionAction.PromoteAtomicReplace;
    }

    /// <summary>
    /// Decide what to do with `runtime/update.exe` relative to the top-level
    /// `update.exe`. Top-level always wins (post-v2.1.3 rule).
    /// </summary>
    public static UpdatePromotionAction DecideRuntimeUpdateExe(UpdateExeContext ctx)
    {
        if (!ctx.IsMainAppHost) return UpdatePromotionAction.Skip;
        if (!ctx.HasRuntimeNestedUpdateExe) return UpdatePromotionAction.Skip;
        if (ctx.HasUpdateExe) return UpdatePromotionAction.DropNested; // top-level wins
        return UpdatePromotionAction.PromoteAtomicReplace;
    }
}

/// <summary>State the promoter consults for its decision.</summary>
public readonly record struct UpdateExeContext(
    bool IsMainAppHost,
    bool HasUpdateExe,
    bool HasUpdateExeNew,
    bool HasRuntimeNestedUpdateExe,
    bool UpdateExeIdenticalToNew);

/// <summary>What the live filesystem adapter should do.</summary>
public enum UpdatePromotionAction
{
    Skip,
    PromoteAtomicReplace,
    DropStaged,
    DropNested,
}
