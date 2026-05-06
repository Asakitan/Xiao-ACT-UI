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

            SharpPcap.ICaptureDevice? picked = null;
            if (!string.IsNullOrEmpty(_deviceName))
            {
                foreach (var d in devices)
                {
                    if (string.Equals(d.Name, _deviceName, StringComparison.OrdinalIgnoreCase))
                    {
                        picked = d;
                        break;
                    }
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
}

public readonly record struct NetworkDeviceInfo(string Name, string Description, string? MacAddress);
