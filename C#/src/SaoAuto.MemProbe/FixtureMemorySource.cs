using System.Buffers.Binary;

namespace SaoAuto.MemProbe;

/// <summary>
/// In-memory <see cref="IMemorySource"/> for tests. Backed by a sparse
/// dictionary of (address → bytes) pages plus an explicit region list.
/// All read methods follow the production contract: missing pages return
/// <c>null</c>; partial reads return <c>null</c>.
/// </summary>
public sealed class FixtureMemorySource : IMemorySource
{
    private readonly SortedDictionary<ulong, byte[]> _pages = new();
    private readonly List<MemoryRegion> _regions = new();

    public int Pid { get; }
    public string ProcessName { get; }

    public FixtureMemorySource(int pid = 1234, string name = "fixture.exe")
    {
        Pid = pid;
        ProcessName = name;
    }

    /// <summary>
    /// Map a contiguous range starting at <paramref name="address"/> with the
    /// given bytes and register an associated <see cref="MemoryRegion"/>.
    /// </summary>
    public void Map(ulong address, byte[] data, uint protect = 0x04 /* RW */, uint type = MemoryRegion.MEM_PRIVATE)
    {
        _pages[address] = data;
        _regions.Add(new MemoryRegion(address, (ulong)data.Length, protect, type));
    }

    public IEnumerable<MemoryRegion> IterRegions(bool onlyReadable = true, bool onlyPrivate = true)
    {
        foreach (var r in _regions)
        {
            if (onlyPrivate && !r.IsPrivate) continue;
            yield return r;
        }
    }

    public byte[]? ReadBytes(ulong addr, int n)
    {
        if (n <= 0) return null;
        var buf = new byte[n];
        var read = ReadInto(addr, buf);
        return read == n ? buf : null;
    }

    public int? ReadInto(ulong addr, Span<byte> destination)
    {
        if (destination.Length == 0) return 0;
        foreach (var (baseAddr, page) in _pages)
        {
            var top = baseAddr + (ulong)page.Length;
            if (addr < baseAddr || addr >= top) continue;
            var offset = (int)(addr - baseAddr);
            var avail = page.Length - offset;
            if (avail < destination.Length) return null;
            page.AsSpan(offset, destination.Length).CopyTo(destination);
            return destination.Length;
        }
        return null;
    }

    public int? ReadI32(ulong addr) { Span<byte> b = stackalloc byte[4]; return ReadInto(addr, b) == 4 ? BinaryPrimitives.ReadInt32LittleEndian(b) : null; }
    public uint? ReadU32(ulong addr) { Span<byte> b = stackalloc byte[4]; return ReadInto(addr, b) == 4 ? BinaryPrimitives.ReadUInt32LittleEndian(b) : null; }
    public long? ReadI64(ulong addr) { Span<byte> b = stackalloc byte[8]; return ReadInto(addr, b) == 8 ? BinaryPrimitives.ReadInt64LittleEndian(b) : null; }
    public ulong? ReadU64(ulong addr) { Span<byte> b = stackalloc byte[8]; return ReadInto(addr, b) == 8 ? BinaryPrimitives.ReadUInt64LittleEndian(b) : null; }
    public ulong? ReadPtr(ulong addr) => ReadU64(addr);

    public void Dispose() { _pages.Clear(); _regions.Clear(); }
}
