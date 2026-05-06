namespace SaoAuto.Core.Packets;

/// <summary>
/// Top-level wire message types from <c>packet_parser.MessageType</c>.
/// The 2-byte BE type word at offset +4 of every game frame carries the
/// zstd flag in the top bit (<c>0x8000</c>) and this enum in the low 15 bits.
/// </summary>
public enum MessageType : ushort
{
    None = 0,
    Call = 1,
    Notify = 2,
    Return = 3,
    Echo = 4,
    FrameUp = 5,
    FrameDown = 6,
}

/// <summary>
/// Subset of <c>packet_parser.NotifyMethod</c> ids that drive parser events
/// today. The full list lives in Python; this projection is intentionally
/// scoped to the methods <see cref="PacketParser"/> currently dispatches.
/// </summary>
public static class NotifyMethod
{
    public const int EnterScene = 0x03;
    public const int SyncNearEntities = 0x06;
    public const int SyncContainerData = 0x15;
    public const int SyncContainerDirtyData = 0x16;
    public const int EnterGame = 0x14;
    public const int SyncDungeonData = 0x17;
    public const int SyncDungeonDirtyData = 0x18;
    public const int SyncSeason = 0x1B;
    public const int NotifyReviveUser = 0x27;
    public const int SyncServerTime = 0x2B;
    public const int SyncNearDeltaInfo = 0x2D;
    public const int SyncToMeDeltaInfo = 0x2E;
    public const int NotifyClientKickOff = 0x31;
    public const int NotifyStartPlayingDungeon = 0x37;
    public const int QteBegin = 0x3001;
    public const int SyncClientUseSkill = 0x3002;
    public const int NotifyBuffChange = 0x3003;
    public const int SyncServerSkillStageEnd = 0x3004;
    public const int SyncServerSkillEnd = 0x3005;
    public const int SyncClientUseSkillWorld = 0x43;
    public const int NotifyAllMemberReady = 0x46;
    public const int NotifyCaptainReady = 0x47;
    public const int EnterMatchResultNtf = 0x48001;
}
