using System.Runtime.CompilerServices;
using System.Threading.Channels;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Live <see cref="IPacketSource"/> backed by SharpPcap. Graceful fallback
/// when Npcap is not installed: <see cref="ListDevices"/> returns an empty
/// list and <see cref="ReadAsync"/> exits immediately. The Phase A
/// fixture source remains the testable production path; this is the
/// shim that pumps real network traffic into <see cref="TcpReassembler"/>.
/// </summary>
public sealed class SharpPcapPacketSource : IPacketSource
{
    private static readonly string[] VirtualKeywords =
    {
        "vmware", "virtualbox", "hyper-v", "zerotier",
        "docker", "wsl", "vethernet", "loopback",
        "npcap loopback", "bluetooth",
        "wan miniport", "network monitor", "miniport",
        "microsoft kernel debug", "teredo", "isatap", "6to4",
        "pptp", "l2tp", "sstp", "pppoe", "ikev2",
        "tunnel", "tap-windows", "wireguard", "vpn",
        "pseudo", "microsoft wi-fi direct",
    };

    private static readonly string[] PreferredNicKeywords =
    {
        "ethernet", "wi-fi", "wifi", "wireless", "802.11",
        "realtek", "intel", "broadcom", "qualcomm", "killer",
        "mediatek", "marvell", "aquantia", "nvidia",
        "gigabit", "gaming",
    };

    private readonly ILogger _log;
    private readonly Channel<RawFrame> _channel;
    private readonly string? _bpfFilter;
    private readonly string? _deviceName;
    private SharpPcap.ICaptureDevice? _device;
    private bool _disposed;

    public SharpPcapPacketSource(
        string? deviceName = null,
        string? bpfFilter = "tcp",
        ILogger<SharpPcapPacketSource>? logger = null,
        int channelCapacity = 4096)
    {
        _deviceName = deviceName;
        _bpfFilter = bpfFilter;
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _channel = Channel.CreateBounded<RawFrame>(new BoundedChannelOptions(channelCapacity)
        {
            SingleReader = true,
            SingleWriter = true,
            FullMode = BoundedChannelFullMode.DropOldest,
        });
    }

    public static IReadOnlyList<NetworkDeviceInfo> ListDevices()
    {
        try
        {
            var devices = SharpPcap.CaptureDeviceList.Instance;
            var list = new List<NetworkDeviceInfo>(devices.Count);
            foreach (var d in devices)
            {
                list.Add(new NetworkDeviceInfo(d.Name, d.Description ?? string.Empty, d.MacAddress?.ToString()));
            }
            return list;
        }
        catch (Exception)
        {
            // Npcap not installed → empty list rather than crash.
            return Array.Empty<NetworkDeviceInfo>();
        }
    }

    /// <summary>
    /// Match a user-requested adapter by exact name, exact description, or
    /// case-insensitive substring on either field. Returns null when the
    /// request does not match any known device.
    /// </summary>
    public static NetworkDeviceInfo? ResolveRequestedDevice(
        IReadOnlyList<NetworkDeviceInfo> devices,
        string? requested)
    {
        if (devices is null) throw new ArgumentNullException(nameof(devices));
        if (string.IsNullOrWhiteSpace(requested)) return null;

        var needle = requested.Trim();
        foreach (var d in devices)
        {
            if (string.Equals(d.Name, needle, StringComparison.OrdinalIgnoreCase)
                || string.Equals(d.Description, needle, StringComparison.OrdinalIgnoreCase))
            {
                return d;
            }
        }
        foreach (var d in devices)
        {
            if (d.Name.Contains(needle, StringComparison.OrdinalIgnoreCase)
                || d.Description.Contains(needle, StringComparison.OrdinalIgnoreCase))
            {
                return d;
            }
        }
        return null;
    }

    /// <summary>
    /// Port of Python's auto-select heuristic: prefer non-virtual adapters,
    /// then score likely physical NIC descriptions above the rest.
    /// </summary>
    public static NetworkDeviceInfo? SelectPreferredDevice(
        IReadOnlyList<NetworkDeviceInfo> devices)
    {
        if (devices is null) throw new ArgumentNullException(nameof(devices));
        if (devices.Count == 0) return null;

        var real = devices
            .Where(d => !VirtualKeywords.Any(k => d.Description.Contains(k, StringComparison.OrdinalIgnoreCase)))
            .ToArray();
        var candidates = real.Length > 0 ? real : devices.ToArray();

        return candidates
            .OrderByDescending(ScoreDevice)
            .ThenBy(d => d.Name, StringComparer.OrdinalIgnoreCase)
            .First();
    }

    public async IAsyncEnumerable<RawFrame> ReadAsync(
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(SharpPcapPacketSource));

        if (!TryOpenDevice())
        {
            yield break;
        }

        try
        {
            await foreach (var frame in _channel.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                yield return frame;
            }
        }
        finally
        {
            StopDevice();
        }
    }

    private bool TryOpenDevice()
    {
        try
        {
            var devices = SharpPcap.CaptureDeviceList.Instance;
            if (devices.Count == 0)
            {
                _log.LogWarning("[Capture] no Npcap/SharpPcap devices found; live capture disabled");
                return false;
            }

            var infos = devices
                .Select(d => new NetworkDeviceInfo(d.Name, d.Description ?? string.Empty, d.MacAddress?.ToString()))
                .ToArray();
            var requested = ResolveRequestedDevice(infos, _deviceName);
            if (requested is null && !string.IsNullOrWhiteSpace(_deviceName))
            {
                _log.LogWarning("[Capture] requested adapter not found: {Requested}; falling back to auto-select", _deviceName);
            }
            var selected = requested ?? SelectPreferredDevice(infos);
            if (selected is null) return false;

            SharpPcap.ICaptureDevice? picked = null;
            foreach (var d in devices)
            {
                if (string.Equals(d.Name, selected.Value.Name, StringComparison.OrdinalIgnoreCase))
                {
                    picked = d;
                    break;
                }
            }
            picked ??= devices[0];

            picked.OnPacketArrival += OnPacketArrival;
            picked.Open(new SharpPcap.DeviceConfiguration
            {
                Mode = SharpPcap.DeviceModes.Promiscuous,
                ReadTimeout = 1000,
            });
            if (!string.IsNullOrEmpty(_bpfFilter))
            {
                picked.Filter = _bpfFilter;
            }
            picked.StartCapture();
            _device = picked;
            _log.LogInformation("[Capture] live source online: {Name}", picked.Name);
            return true;
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[Capture] failed to open SharpPcap device; live capture disabled");
            return false;
        }
    }

    private void OnPacketArrival(object sender, SharpPcap.PacketCapture e)
    {
        try
        {
            var raw = e.Data.ToArray();
            var ts = DateTimeOffset.FromUnixTimeMilliseconds(
                (long)(e.Header.Timeval.Seconds * 1000 + e.Header.Timeval.MicroSeconds / 1000));
            _channel.Writer.TryWrite(new RawFrame(ts, raw));
        }
        catch (Exception ex)
        {
            _log.LogDebug(ex, "[Capture] frame copy failed");
        }
    }

    private void StopDevice()
    {
        if (_device is null) return;
        try
        {
            _device.OnPacketArrival -= OnPacketArrival;
            _device.StopCapture();
            _device.Close();
        }
        catch
        {
            // best effort
        }
        _device = null;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        StopDevice();
        _channel.Writer.TryComplete();
    }

    private static int ScoreDevice(NetworkDeviceInfo device)
    {
        var desc = device.Description ?? string.Empty;
        var score = 0;
        foreach (var keyword in PreferredNicKeywords)
        {
            if (desc.Contains(keyword, StringComparison.OrdinalIgnoreCase))
            {
                score++;
            }
        }
        return score;
    }
}

public readonly record struct NetworkDeviceInfo(string Name, string Description, string? MacAddress);
