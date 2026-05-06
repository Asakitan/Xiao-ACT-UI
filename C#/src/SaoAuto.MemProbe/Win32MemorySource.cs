using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.MemProbe;

/// <summary>
/// Live <see cref="IMemorySource"/> backed by Win32
/// <c>OpenProcess</c> + <c>VirtualQueryEx</c> + <c>ReadProcessMemory</c>.
/// Mirrors <c>tools/mem_probe/process.py StarProcess</c>. Read failures
/// (unmapped pages, race with the GC, etc.) return <c>null</c> rather
/// than throwing — that mirrors Python and lets scan paths sweep noisy
/// regions cheaply.
/// </summary>
public sealed class Win32MemorySource : IMemorySource
{
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_READ = 0x0010;

    private const uint MEM_COMMIT = 0x1000;
    private const uint PAGE_NOACCESS = 0x01;
    private const uint PAGE_GUARD = 0x100;

    private const uint READABLE_PROTECTS =
        0x02 /* RO   */ | 0x04 /* RW    */ | 0x08 /* WC */ |
        0x20 /* XR   */ | 0x40 /* XRW   */ | 0x80 /* XWC */;

    private readonly ILogger _log;
    private IntPtr _handle;
    private readonly string _name;
    private readonly int _pid;
    private bool _disposed;

    public int Pid => _pid;
    public string ProcessName => _name;

    public Win32MemorySource(string processName, ILogger<Win32MemorySource>? logger = null)
    {
        _log = (ILogger?)logger ?? NullLogger.Instance;
        var trimmed = processName ?? throw new ArgumentNullException(nameof(processName));
        if (trimmed.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            trimmed = trimmed[..^4];

        var procs = Process.GetProcessesByName(trimmed);
        if (procs.Length == 0)
            throw new InvalidOperationException($"Process not found: {processName}");

        var p = procs[0];
        for (var i = 1; i < procs.Length; i++) procs[i].Dispose();
        _pid = p.Id;
        _name = p.ProcessName;
        p.Dispose();

        _handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, _pid);
        if (_handle == IntPtr.Zero)
            throw new InvalidOperationException(
                $"OpenProcess failed for {_name} (pid={_pid}); admin privileges may be required.");
    }

    public IEnumerable<MemoryRegion> IterRegions(bool onlyReadable = true, bool onlyPrivate = true)
    {
        ulong addr = 0;
        const ulong maxAddr = 0x7FFF_FFFF_FFFFUL;
        var size = (uint)Marshal.SizeOf<MEMORY_BASIC_INFORMATION64>();
        while (addr < maxAddr && !_disposed)
        {
            var ret = VirtualQueryEx(_handle, (IntPtr)addr, out var mbi, size);
            if (ret == UIntPtr.Zero) yield break;
            if (mbi.RegionSize == 0) yield break;
            var next = mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT)
            {
                var ok = true;
                if (onlyReadable)
                {
                    if ((mbi.Protect & PAGE_GUARD) != 0) ok = false;
                    else if ((mbi.Protect & PAGE_NOACCESS) != 0) ok = false;
                    else if ((mbi.Protect & READABLE_PROTECTS) == 0) ok = false;
                }
                if (ok && onlyPrivate && (mbi.Type & MemoryRegion.MEM_PRIVATE) == 0) ok = false;
                if (ok) yield return new MemoryRegion(mbi.BaseAddress, mbi.RegionSize, mbi.Protect, mbi.Type);
            }
            if (next <= addr) yield break;
            addr = next;
        }
    }

    public byte[]? ReadBytes(ulong addr, int n)
    {
        if (n <= 0 || _disposed) return null;
        var buf = new byte[n];
        var read = ReadInto(addr, buf);
        if (read is null || read.Value != n) return null;
        return buf;
    }

    public int? ReadInto(ulong addr, Span<byte> destination)
    {
        if (destination.Length == 0 || _disposed) return 0;
        unsafe
        {
            fixed (byte* p = destination)
            {
                if (!ReadProcessMemory(_handle, (IntPtr)addr, (IntPtr)p, (UIntPtr)destination.Length, out var read))
                    return null;
                return (int)read;
            }
        }
    }

    public int? ReadI32(ulong addr)
    {
        Span<byte> b = stackalloc byte[4];
        return ReadInto(addr, b) is 4 ? BinaryPrimitives.ReadInt32LittleEndian(b) : null;
    }

    public uint? ReadU32(ulong addr)
    {
        Span<byte> b = stackalloc byte[4];
        return ReadInto(addr, b) is 4 ? BinaryPrimitives.ReadUInt32LittleEndian(b) : null;
    }

    public long? ReadI64(ulong addr)
    {
        Span<byte> b = stackalloc byte[8];
        return ReadInto(addr, b) is 8 ? BinaryPrimitives.ReadInt64LittleEndian(b) : null;
    }

    public ulong? ReadU64(ulong addr)
    {
        Span<byte> b = stackalloc byte[8];
        return ReadInto(addr, b) is 8 ? BinaryPrimitives.ReadUInt64LittleEndian(b) : null;
    }

    public ulong? ReadPtr(ulong addr) => ReadU64(addr);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_handle != IntPtr.Zero)
        {
            CloseHandle(_handle);
            _handle = IntPtr.Zero;
        }
    }

    public static bool IsAdmin()
    {
        try
        {
            using var id = System.Security.Principal.WindowsIdentity.GetCurrent();
            return new System.Security.Principal.WindowsPrincipal(id)
                .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MEMORY_BASIC_INFORMATION64
    {
        public ulong BaseAddress;
        public ulong AllocationBase;
        public uint AllocationProtect;
        public uint __alignment1;
        public ulong RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
        public uint __alignment2;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQueryEx(IntPtr handle, IntPtr addr, out MEMORY_BASIC_INFORMATION64 mbi, uint dwLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReadProcessMemory(IntPtr handle, IntPtr addr, IntPtr buffer, UIntPtr size, out UIntPtr read);
}
