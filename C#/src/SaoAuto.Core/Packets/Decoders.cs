using Google.Protobuf;
using Star;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Decoder for <c>NotifyMethod.SyncServerTime (0x2B)</c>. Mirrors
/// <c>packet_parser._on_sync_server_time</c>: parses the proto, computes
/// the offset between server-reported and local wall clock, and emits a
/// <see cref="ServerTimeEvent"/>. Used by cooldown / DPS / boss-HP code
/// to project packet timestamps onto game time.
/// </summary>
public sealed class SyncServerTimeDecoder : ProtoMethodDecoder<SyncServerTime>
{
    public override int MethodId => NotifyMethod.SyncServerTime;
    protected override MessageParser<SyncServerTime> Parser => SyncServerTime.Parser;

    protected override void Project(SyncServerTime msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (msg.ClientMilliseconds <= 0 && msg.ServerMilliseconds <= 0) return;
        var localMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var offset = (double)msg.ServerMilliseconds - localMs;
        emit(new ServerTimeEvent((ulong)msg.ServerMilliseconds, offset, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.NotifyReviveUser (0x27)</c>. Emits a
/// <see cref="ReviveEvent"/> with the resurrected actor's uuid.
/// </summary>
public sealed class NotifyReviveUserDecoder : ProtoMethodDecoder<NotifyReviveUser>
{
    public override int MethodId => NotifyMethod.NotifyReviveUser;
    protected override MessageParser<NotifyReviveUser> Parser => NotifyReviveUser.Parser;

    protected override void Project(NotifyReviveUser msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new ReviveEvent(unchecked((ulong)msg.VActorUuid), timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyClientKickOff (0x31)</c>.
/// Server-side session-end notifications carry a small empty message —
/// we surface a <see cref="KickOffEvent"/> so subscribers can tear down
/// state without having to walk the proto.
/// </summary>
public sealed class NotifyClientKickOffDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyClientKickOff;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new KickOffEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyAllMemberReady (0x46)</c>.
/// </summary>
public sealed class NotifyAllMemberReadyDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyAllMemberReady;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new AllMemberReadyEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.NotifyCaptainReady (0x47)</c>.
/// </summary>
public sealed class NotifyCaptainReadyDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyCaptainReady;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new CaptainReadyEvent(timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.NotifyStartPlayingDungeon (0x37)</c>.
/// The proto class is not generated as a named message (the body is a
/// single varint dungeon id), so we read it manually with
/// <see cref="CyPacketExtras.DecodeFields"/>.
/// </summary>
public sealed class NotifyStartPlayingDungeonDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.NotifyStartPlayingDungeon;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (body.IsEmpty) { emit(new DungeonStartEvent(0, timestampSeconds)); return; }
        var fields = CyPacketExtras.DecodeFields(body.ToArray());
        var dungeonId = 0;
        if (fields.TryGetValue(1, out var list) && list.Count > 0)
        {
            dungeonId = list[0] switch
            {
                ulong u => Varint.ToInt32(u),
                long l => unchecked((int)l),
                int i => i,
                _ => 0,
            };
        }
        emit(new DungeonStartEvent(dungeonId, timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.EnterScene (0x03)</c>. Surfaces a
/// <see cref="EnterSceneEvent"/> so subscribers can refresh per-scene
/// state (DPS reset, recognition prime, etc.) without inspecting the
/// proto body.
/// </summary>
public sealed class EnterSceneDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.EnterScene;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new EnterSceneEvent(timestampSeconds));
    }
}

/// <summary>
/// Marker decoder for <c>NotifyMethod.EnterMatchResultNtf (0x48001)</c>.
/// Fires after a dungeon clear — useful to drive the DPS finalize gate
/// and to flip the auto-key engine into post-combat mode.
/// </summary>
public sealed class EnterMatchResultDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.EnterMatchResultNtf;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new MatchResultEvent(timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.EnterGame (0x14)</c>. Mirrors
/// <c>packet_parser._on_enter_game</c>: pulls the int64 uid (field 1)
/// and the optional server-name string (field 2) out of the manually
/// decoded fields. Surfaces an <see cref="EnterGameEvent"/> so the
/// bridge layer can confirm the local player's UID and reset
/// per-session caches.
/// </summary>
public sealed class EnterGameDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.EnterGame;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (body.IsEmpty) return;
        var fields = CyPacketExtras.DecodeFields(body.ToArray());
        ulong uid = 0;
        if (fields.TryGetValue(1, out var list) && list.Count > 0)
        {
            uid = list[0] switch
            {
                ulong u => u,
                long l => unchecked((ulong)l),
                int i => unchecked((ulong)(long)i),
                _ => 0,
            };
        }
        if (uid == 0) return;
        emit(new EnterGameEvent(uid, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.NotifyBuffChange (0x3003)</c>. Emits a
/// <see cref="BuffChangeEvent"/> with the (old, new) buff ids; full buff
/// state is tracked separately from <c>AoiSyncDelta.BuffInfos</c>.
/// </summary>
public sealed class NotifyBuffChangeDecoder : ProtoMethodDecoder<NotifyBuffChange>
{
    public override int MethodId => NotifyMethod.NotifyBuffChange;
    protected override MessageParser<NotifyBuffChange> Parser => NotifyBuffChange.Parser;

    protected override void Project(NotifyBuffChange msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new BuffChangeEvent(msg.OldBuffId, msg.NewBuffId, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncClientUseSkill (0x3002)</c>. The
/// caster is implicitly the local player; the payload carries only the
/// (target, skill_level_id) pair. Bridge layer is responsible for
/// stamping the per-player skill-use timestamp keyed off the current
/// uid.
/// </summary>
public sealed class SyncClientUseSkillDecoder : ProtoMethodDecoder<SyncClientUseSkill>
{
    public override int MethodId => NotifyMethod.SyncClientUseSkill;
    protected override MessageParser<SyncClientUseSkill> Parser => SyncClientUseSkill.Parser;

    protected override void Project(SyncClientUseSkill msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new SkillUseEvent(msg.SkillTargetUuid, msg.SkillLevelId, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncServerSkillEnd (0x3005)</c>. Fires
/// when a skill cast resolves; surfaces the per-cast SkillUuid only.
/// </summary>
public sealed class SyncServerSkillEndDecoder : ProtoMethodDecoder<SyncServerSkillEnd>
{
    public override int MethodId => NotifyMethod.SyncServerSkillEnd;
    protected override MessageParser<SyncServerSkillEnd> Parser => SyncServerSkillEnd.Parser;

    protected override void Project(SyncServerSkillEnd msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new SkillEndEvent(msg.SkillUuid, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncServerSkillStageEnd (0x3004)</c>.
/// Outer message wraps a <c>ServerSkillStageEnd</c> inner with stage
/// transition metadata. We unwrap and emit a single event so subscribers
/// don't have to traverse the proto.
/// </summary>
public sealed class SyncServerSkillStageEndDecoder : ProtoMethodDecoder<SyncServerSkillStageEnd>
{
    public override int MethodId => NotifyMethod.SyncServerSkillStageEnd;
    protected override MessageParser<SyncServerSkillStageEnd> Parser => SyncServerSkillStageEnd.Parser;

    protected override void Project(SyncServerSkillStageEnd msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var info = msg.SkillStageEndInfo;
        if (info is null) return;
        emit(new SkillStageEndEvent(
            info.SkillUuid, info.StageId, info.NewStageId, info.ConditionId,
            timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.QteBegin (0x3001)</c>. The proto class is
/// not generated as a named message, so we read fields manually:
/// field 1 → qte_id, field 2 → qte_type. Both are surfaced as long to
/// preserve the full varint range.
/// </summary>
public sealed class QteBeginDecoder : IMethodDecoder
{
    public int MethodId => NotifyMethod.QteBegin;

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (body.IsEmpty) { emit(new QteBeginEvent(0, 0, timestampSeconds)); return; }
        var fields = CyPacketExtras.DecodeFields(body.ToArray());
        long qteId = ExtractInt64(fields, 1);
        long qteType = ExtractInt64(fields, 2);
        emit(new QteBeginEvent(qteId, qteType, timestampSeconds));
    }

    private static long ExtractInt64(Dictionary<int, List<object>> fields, int tag)
    {
        if (!fields.TryGetValue(tag, out var list) || list.Count == 0) return 0;
        return list[0] switch
        {
            ulong u => unchecked((long)u),
            long l => l,
            int i => i,
            _ => 0,
        };
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncDungeonData (0x17)</c>. Mirrors
/// <c>packet_parser._on_sync_dungeon_data</c>: pulls the inner
/// <c>DungeonSyncData</c>, surfaces (scene_uuid, dungeon_difficulty,
/// target_progress[]). Side effects (dungeon-id mutation,
/// soft-scene-transition fires, target-reset rules) belong to the
/// bridge layer — the decoder only emits the data shape.
/// </summary>
public sealed class SyncDungeonDataDecoder : ProtoMethodDecoder<SyncDungeonData>
{
    public override int MethodId => NotifyMethod.SyncDungeonData;
    protected override MessageParser<SyncDungeonData> Parser => SyncDungeonData.Parser;

    protected override void Project(SyncDungeonData msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var vd = msg.VData;
        if (vd is null) return;
        long sceneUuid = vd.SceneUuid;
        int difficulty = 0;
        if (vd.DungeonSceneInfo is not null)
        {
            difficulty = vd.DungeonSceneInfo.Difficulty;
        }
        var targets = new List<DungeonTargetProgress>();
        if (vd.Target is not null)
        {
            foreach (var kv in vd.Target.TargetData)
            {
                var td = kv.Value;
                if (td is null) continue;
                targets.Add(new DungeonTargetProgress(td.TargetId, td.Nums, td.Complete));
            }
        }
        emit(new DungeonDataEvent(sceneUuid, difficulty, targets, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncDungeonDirtyData (0x18)</c>. Mirrors
/// <c>packet_parser._on_sync_dungeon_dirty_data</c>: unwraps the
/// <c>BufferStream</c>, runs the padded-i32 parser via
/// <see cref="CyPacketExtras.ParseDungeonDirtyBuffer"/>, surfaces
/// (flow_state, targets[]). Drops on empty buffer or malformed inner
/// buffer to mirror Python's <c>try/except</c> swallow.
/// </summary>
public sealed class SyncDungeonDirtyDataDecoder : ProtoMethodDecoder<SyncDungeonDirtyData>
{
    public override int MethodId => NotifyMethod.SyncDungeonDirtyData;
    protected override MessageParser<SyncDungeonDirtyData> Parser => SyncDungeonDirtyData.Parser;

    protected override void Project(SyncDungeonDirtyData msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var vd = msg.VData;
        if (vd is null) return;
        var bytes = vd.Buffer;
        if (bytes is null || bytes.Length == 0) return;
        try
        {
            var (flowState, targets) = CyPacketExtras.ParseDungeonDirtyBuffer(bytes.ToByteArray());
            emit(new DungeonDirtyDataEvent(flowState, targets, timestampSeconds));
        }
        catch (InvalidDataException)
        {
            // mirror Python's debug-log + drop on bad inner buffer
        }
    }
}


/// <summary>
/// Alias decoder for <c>NotifyMethod.SyncClientUseSkillWorld (0x43)</c>.
/// Python routes both ids to the same handler; the proto type is shared
/// (no <c>SyncClientUseSkillWorld</c> message exists in the wire schema).
/// Reuses <see cref="SyncClientUseSkill"/> for parsing and emits the same
/// <see cref="SkillUseEvent"/> shape so bridge code doesn't need a 2nd
/// switch arm.
/// </summary>
public sealed class SyncClientUseSkillWorldDecoder : ProtoMethodDecoder<SyncClientUseSkill>
{
    public override int MethodId => NotifyMethod.SyncClientUseSkillWorld;
    protected override MessageParser<SyncClientUseSkill> Parser => SyncClientUseSkill.Parser;

    protected override void Project(SyncClientUseSkill msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        emit(new SkillUseEvent(msg.SkillTargetUuid, msg.SkillLevelId, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncNearEntities (0x06)</c>. Surfaces
/// (uuid, type) for each appearing entity and (uuid, reason) for each
/// disappearing entity. Full per-entity attribute mutation lives in
/// Python's ~150-LOC handler — that's a follow-up bridge session; this
/// decoder keeps the boundary cost low and lets a consumer count or
/// snapshot entities by id.
/// </summary>
public sealed class SyncNearEntitiesDecoder : ProtoMethodDecoder<SyncNearEntities>
{
    public override int MethodId => NotifyMethod.SyncNearEntities;
    protected override MessageParser<SyncNearEntities> Parser => SyncNearEntities.Parser;

    protected override void Project(SyncNearEntities msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var appear = new List<EntityAppearance>(msg.Appear?.Count ?? 0);
        if (msg.Appear is not null)
        {
            foreach (var e in msg.Appear)
            {
                appear.Add(new EntityAppearance(e.Uuid, (int)e.EntType));
            }
        }
        var disappear = new List<EntityDisappearance>(msg.Disappear?.Count ?? 0);
        if (msg.Disappear is not null)
        {
            foreach (var d in msg.Disappear)
            {
                disappear.Add(new EntityDisappearance(d.Uuid, (int)d.Type));
            }
        }
        if (appear.Count == 0 && disappear.Count == 0) return;
        emit(new NearEntitiesEvent(appear, disappear, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncNearDeltaInfo (0x2D)</c>. Surfaces the
/// list of entity uuids that received a delta update on this tick.
/// Per-attr translation is held until the bridge layer needs it.
/// </summary>
public sealed class SyncNearDeltaInfoDecoder : ProtoMethodDecoder<SyncNearDeltaInfo>
{
    public override int MethodId => NotifyMethod.SyncNearDeltaInfo;
    protected override MessageParser<SyncNearDeltaInfo> Parser => SyncNearDeltaInfo.Parser;

    protected override void Project(SyncNearDeltaInfo msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        if (msg.DeltaInfos is null || msg.DeltaInfos.Count == 0) return;
        var uuids = new List<long>(msg.DeltaInfos.Count);
        foreach (var d in msg.DeltaInfos)
        {
            uuids.Add(d.Uuid);
        }
        emit(new NearDeltaEvent(uuids, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncToMeDeltaInfo (0x2E)</c>. The
/// "to-me" delta carries the local player's per-tick attribute snapshot
/// + hate list + skill cooldown list. This skeleton emits (uuid, hate
/// count, skill cd count); deeper destructuring is a follow-up.
/// </summary>
public sealed class SyncToMeDeltaInfoDecoder : ProtoMethodDecoder<SyncToMeDeltaInfo>
{
    public override int MethodId => NotifyMethod.SyncToMeDeltaInfo;
    protected override MessageParser<SyncToMeDeltaInfo> Parser => SyncToMeDeltaInfo.Parser;

    protected override void Project(SyncToMeDeltaInfo msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var d = msg.DeltaInfo;
        if (d is null) return;
        var hateCount = d.SyncHateIds?.Count ?? 0;
        var cds = new List<SkillCdSnapshot>();
        if (d.SyncSkillCDs is not null)
        {
            foreach (var cd in d.SyncSkillCDs)
            {
                if (cd is null) continue;
                // Mirror Python: prefer ValidCDTimeLegacy when > 0, else ValidCDTime.
                var vcd = cd.ValidCDTimeLegacy > 0 ? cd.ValidCDTimeLegacy : cd.ValidCDTime;
                cds.Add(new SkillCdSnapshot(
                    SkillLevelId: cd.SkillLevelId,
                    BeginMs: cd.SkillBeginTime,
                    DurationMs: cd.Duration,
                    ValidCdTimeMs: vcd,
                    ChargeCount: cd.ChargeCount,
                    SubCdRatio: cd.SubCDRatio,
                    SubCdFixed: cd.SubCDFixed,
                    AccelerateCdRatio: cd.AccelerateCDRatio));
            }
        }
        emit(new ToMeDeltaEvent(d.Uuid, hateCount, cds, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncContainerData (0x15)</c>. Mirrors
/// the top-level identity + HP destructuring at the head of Python's
/// <c>_on_sync_container_data</c>: pulls CharId, name, level, fight
/// point, cur/max HP, and the float OriginEnergy out of the
/// <c>CharSerialize</c> wrapped in <c>VData</c>. Drops when CharId is
/// missing or non-positive (matches Python's `if not char_id:` guard).
///
/// Skeleton-only: the deeper attribute table (~600 LOC of inventory /
/// equipment / per-attr translation) is intentionally not ported here.
/// When a consumer needs more fields, expand <see cref="ContainerSyncEvent"/>
/// or surface a sibling event from this decoder rather than mutating
/// the existing record (positional ctor changes are breaking).
/// </summary>
public sealed class SyncContainerDataDecoder : ProtoMethodDecoder<SyncContainerData>
{
    public override int MethodId => NotifyMethod.SyncContainerData;
    protected override MessageParser<SyncContainerData> Parser => SyncContainerData.Parser;

    protected override void Project(SyncContainerData msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var ch = msg.VData;
        if (ch is null) return;
        var charId = ch.CharId;
        if (charId <= 0) return;
        var name = ch.CharBase?.Name ?? string.Empty;
        var fightPoint = ch.CharBase?.FightPoint ?? 0;
        var level = ch.RoleLevel?.Level ?? 0;
        long curHp = 0;
        long maxHp = 0;
        float energy = 0f;
        if (ch.Attr is not null)
        {
            curHp = ch.Attr.CurHp;
            maxHp = ch.Attr.MaxHp;
            energy = ch.Attr.OriginEnergy;
        }
        emit(new ContainerSyncEvent(
            charId, name, level, fightPoint, curHp, maxHp, energy, timestampSeconds));
    }
}

/// <summary>
/// Decoder for <c>NotifyMethod.SyncContainerDirtyData (0x16)</c>. Mirrors
/// the head of <c>packet_parser._on_sync_container_dirty</c>: unwraps the
/// proto <see cref="SyncContainerDirtyData.VData"/> -> <c>BufferStream</c>
/// -> bytes, then parses the V3.3.6 custom binary stream via
/// <see cref="CyPacketExtras.ParseContainerDirtyStream"/>. Emits a single
/// <see cref="ContainerDirtyEvent"/> when the (field_index, sub_field)
/// pair is in the supported subset; drops silently on unrecognised
/// fields, short buffers, or bad header tags (matches Python).
///
/// Skeleton-only: SeasonCenter (50) / SeasonMedalInfo (52) /
/// MonsterHuntInfo (56) / ProfessionList (61) routes are deferred until
/// GameState exposes the corresponding slots.
/// </summary>
public sealed class SyncContainerDirtyDataDecoder : ProtoMethodDecoder<SyncContainerDirtyData>
{
    public override int MethodId => NotifyMethod.SyncContainerDirtyData;
    protected override MessageParser<SyncContainerDirtyData> Parser => SyncContainerDirtyData.Parser;

    protected override void Project(SyncContainerDirtyData msg, double timestampSeconds, Action<ParserEvent> emit)
    {
        var bs = msg.VData;
        if (bs is null) return;
        var bytes = bs.Buffer;
        if (bytes is null || bytes.Length < 12) return;
        var change = CyPacketExtras.ParseContainerDirtyStream(bytes.ToByteArray());
        if (change is null) return;
        emit(new ContainerDirtyEvent(change, timestampSeconds));
    }
}
