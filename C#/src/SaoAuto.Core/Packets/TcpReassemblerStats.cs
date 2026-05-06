namespace SaoAuto.Core.Packets;

/// <summary>
/// Snapshot of <see cref="TcpReassembler"/> stats. Mirrors keys of Python's
/// <c>TcpReassembler.stats</c> dict so logs and bug reports map across the port.
/// </summary>
public sealed record TcpReassemblerStats(
    long RawFrames,
    long TcpSegments,
    long CompleteGameFrames,
    long SeqResets,
    long CacheOverflows,
    long GapSkips,
    long ServerChanges,
    long ReplayedAfterChange,
    long ForceReconnects);
