namespace SaoAuto.MemProbe;

/// <summary>
/// One committed virtual memory region of a target process. Mirrors
/// Python <c>tools.mem_probe.process.MemoryRegion</c>.
/// </summary>
public readonly record struct MemoryRegion(ulong Base, ulong Size, uint Protect, uint Type)
{
    public const uint MEM_PRIVATE = 0x20000;
    public const uint MEM_IMAGE = 0x1000000;
    public const uint MEM_MAPPED = 0x40000;
    public bool IsPrivate => (Type & MEM_PRIVATE) != 0;
    public bool IsImage => (Type & MEM_IMAGE) != 0;
}

/// <summary>
/// Read-only contract for attaching to a target process and reading its
/// memory pages. Mirrors the surface of Python <c>StarProcess</c> /
/// <c>pymem.Pymem</c> as consumed by <c>mem_probe.locator</c> and the
/// watcher set. Implementations:
///   - <see cref="Win32MemorySource"/> — live <c>ReadProcessMemory</c>
///   - <see cref="FixtureMemorySource"/> (tests) — in-memory replay
/// All read methods MUST return <c>null</c> on failure (touching an
/// unmapped page is normal during scanning); they MUST NOT throw.
/// </summary>
public interface IMemorySource : IDisposable
{
    int Pid { get; }
    string ProcessName { get; }

    IEnumerable<MemoryRegion> IterRegions(bool onlyReadable = true, bool onlyPrivate = true);
    byte[]? ReadBytes(ulong addr, int n);
    int? ReadInto(ulong addr, Span<byte> destination);

    int? ReadI32(ulong addr);
    uint? ReadU32(ulong addr);
    long? ReadI64(ulong addr);
    ulong? ReadU64(ulong addr);
    ulong? ReadPtr(ulong addr);
}
