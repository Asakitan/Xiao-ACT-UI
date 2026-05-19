using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S133 — adapter selection parity. Mirrors the Python capture bridge:
/// honour `capture_device` by exact/substring match and otherwise prefer
/// a likely physical NIC over loopback / VMware / tunnel adapters.
/// </summary>
public class Session133PacketSourceSelectionTests
{
    [Fact]
    public void ResolveRequestedDevice_ExactNameWins()
    {
        var devices = new[]
        {
            new NetworkDeviceInfo(@"\Device\NPF_VMWARE", "VMware Virtual Ethernet Adapter", null),
            new NetworkDeviceInfo(@"\Device\NPF_REALTEK", "Realtek Gaming 2.5GbE", null),
        };

        var picked = SharpPcapPacketSource.ResolveRequestedDevice(devices, @"\Device\NPF_REALTEK");

        Assert.NotNull(picked);
        Assert.Equal(@"\Device\NPF_REALTEK", picked!.Value.Name);
    }

    [Fact]
    public void ResolveRequestedDevice_SubstringMatchesDescription()
    {
        var devices = new[]
        {
            new NetworkDeviceInfo(@"\Device\NPF_WIFI", "Intel(R) Wi-Fi 6 AX201 160MHz", null),
            new NetworkDeviceInfo(@"\Device\NPF_ETH", "Realtek PCIe GbE Family Controller", null),
        };

        var picked = SharpPcapPacketSource.ResolveRequestedDevice(devices, "Wi-Fi 6");

        Assert.NotNull(picked);
        Assert.Equal(@"\Device\NPF_WIFI", picked!.Value.Name);
    }

    [Fact]
    public void SelectPreferredDevice_SkipsVirtualAdaptersWhenPossible()
    {
        var devices = new[]
        {
            new NetworkDeviceInfo(@"\Device\NPF_LOOP", "Npcap Loopback Adapter", null),
            new NetworkDeviceInfo(@"\Device\NPF_VMNET", "VMware Virtual Ethernet Adapter", null),
            new NetworkDeviceInfo(@"\Device\NPF_REALTEK", "Realtek Gaming 2.5GbE Family Controller", null),
        };

        var picked = SharpPcapPacketSource.SelectPreferredDevice(devices);

        Assert.NotNull(picked);
        Assert.Equal(@"\Device\NPF_REALTEK", picked!.Value.Name);
    }

    [Fact]
    public void SelectPreferredDevice_FallsBackWhenOnlyVirtualAdaptersExist()
    {
        var devices = new[]
        {
            new NetworkDeviceInfo(@"\Device\NPF_LOOP", "Npcap Loopback Adapter", null),
            new NetworkDeviceInfo(@"\Device\NPF_VMNET", "VMware Virtual Ethernet Adapter", null),
        };

        var picked = SharpPcapPacketSource.SelectPreferredDevice(devices);

        Assert.NotNull(picked);
        Assert.Equal(@"\Device\NPF_VMNET", picked!.Value.Name);
    }
}
