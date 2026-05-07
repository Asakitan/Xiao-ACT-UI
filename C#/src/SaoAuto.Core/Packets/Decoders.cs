using Google.Protobuf;
using SaoAuto.Core.Automation;
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
                var attrs = ExtractMonsterCoreAttrs(e.Attrs);
                appear.Add(new EntityAppearance(e.Uuid, (int)e.EntType)
                {
                    CurHp = attrs.CurHp,
                    MaxHp = attrs.MaxHp,
                    Level = attrs.Level,
                    Name = attrs.Name,
                    TemplateId = attrs.TemplateId,
                    Attrs = attrs,
                });
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

    // S110/S111/S112/S114/S115: AttrCollection unpack for monster
    // attrs. Mirrors Python's _process_monster_attr_collection at
    // packet_parser.py 4259+ — recognised ids:
    //   1     NAME (length-delimited utf-8)
    //   10    ID (template_id)
    //   441/440  EXTINCTION / MAX_EXTINCTION (break-bar HP)
    //   443/442  STUNNED / MAX_STUNNED (stun gauge)
    //   444   IN_OVERDRIVE (boss enrage flag)
    //   455   BREAKING_STAGE (break phase)
    //   10000 LEVEL
    //   11310/11320  HP / MAX_HP
    // Empty/null inputs return all-default so the decoder stays no-op-
    // friendly for entities/deltas that omit Attrs (e.g. players).
    // Skip-on-empty matches Python's
    // `if not raw_data or not attr_id: continue`.
    // S115: returns the public MonsterCoreAttrs struct (was internal in
    // S110-S114). Stop adding init slots to the events — new attrs go
    // straight into this struct.
    internal static MonsterCoreAttrs ExtractMonsterCoreAttrs(AttrCollection? attrs)
    {
        if (attrs is null || attrs.Attrs.Count == 0) return default;
        int curHp = 0, maxHp = 0, level = 0, templateId = 0;
        int breakingStage = 0, extinction = 0, maxExtinction = 0;
        int stunned = 0, maxStunned = 0;
        int shieldTotal = 0, shieldMaxTotal = 0;
        bool inOverdrive = false;
        bool hasCurHp = false, hasMaxHp = false, hasLevel = false;
        bool hasName = false, hasTemplateId = false;
        bool hasBreakingStage = false, hasExtinction = false, hasMaxExtinction = false;
        bool hasStunned = false, hasMaxStunned = false, hasInOverdrive = false;
        bool hasShield = false;
        var name = string.Empty;
        // S117: extended monster flags / attrs.
        bool isLockStunned = false, stopBreakingTicking = false, firstAttack = false;
        int state = 0, deadType = 0, deadTime = 0;
        long hatedCharId = 0;
        var hatedCharName = string.Empty;
        bool hasIsLockStunned = false, hasStopBreakingTicking = false;
        bool hasState = false, hasDeadType = false, hasDeadTime = false;
        bool hasFirstAttack = false, hasHatedCharId = false, hasHatedCharName = false;
        foreach (var attr in attrs.Attrs)
        {
            var id = attr.Id;
            var raw = attr.RawData;
            if (id == 0 || raw is null || raw.Length == 0) continue;
            switch (id)
            {
                case 1: // AttrType.NAME — length-delimited utf-8 string
                    name = CyPacketExtras.DecodeStringFromRaw(raw.Span);
                    hasName = true;
                    break;
                case 10: // AttrType.ID — template_id varint
                    templateId = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasTemplateId = true;
                    break;
                case 440: // AttrType.MAX_EXTINCTION
                    maxExtinction = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasMaxExtinction = true;
                    break;
                case 441: // AttrType.EXTINCTION
                    extinction = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasExtinction = true;
                    break;
                case 442: // AttrType.MAX_STUNNED
                    maxStunned = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasMaxStunned = true;
                    break;
                case 443: // AttrType.STUNNED
                    stunned = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasStunned = true;
                    break;
                case 444: // AttrType.IN_OVERDRIVE — bool flag (varint)
                    inOverdrive = CyPacketExtras.RawVarintToInt32(raw.Span) != 0;
                    hasInOverdrive = true;
                    break;
                case 455: // AttrType.BREAKING_STAGE
                    breakingStage = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasBreakingStage = true;
                    break;
                case 11310: // AttrType.HP
                    curHp = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasCurHp = true;
                    break;
                case 11320: // AttrType.MAX_HP
                    maxHp = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasMaxHp = true;
                    break;
                case 10000: // AttrType.LEVEL
                    level = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasLevel = true;
                    break;
                case 60050: // AttrType.SHIELD_LIST — repeated ShieldInfo submessage
                    {
                        var (total, maxTotal) = CyPacketExtras.DecodeShieldList(raw.Span);
                        shieldTotal = total;
                        shieldMaxTotal = maxTotal;
                        hasShield = true;
                    }
                    break;
                case 11: // AttrType.STATE
                    state = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasState = true;
                    break;
                case 78: // AttrType.DEAD_TYPE
                    deadType = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasDeadType = true;
                    break;
                case 206: // AttrType.DEAD_TIME
                    deadTime = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasDeadTime = true;
                    break;
                case 445: // AttrType.IS_LOCK_STUNNED — bool varint
                    isLockStunned = CyPacketExtras.RawVarintToInt32(raw.Span) != 0;
                    hasIsLockStunned = true;
                    break;
                case 453: // AttrType.STOP_BREAKING_TICKING — bool varint
                    stopBreakingTicking = CyPacketExtras.RawVarintToInt32(raw.Span) != 0;
                    hasStopBreakingTicking = true;
                    break;
                case 456: // AttrType.FIRST_ATTACK — bool varint
                    firstAttack = CyPacketExtras.RawVarintToInt32(raw.Span) != 0;
                    hasFirstAttack = true;
                    break;
                case 471: // AttrType.HATED_CHAR_ID — int64 varint
                    {
                        var u = Varint.ReadUInt64(raw.Span, out _);
                        hatedCharId = unchecked((long)u);
                        hasHatedCharId = true;
                    }
                    break;
                case 473: // AttrType.HATED_CHAR_NAME — length-delimited utf-8
                    hatedCharName = CyPacketExtras.DecodeStringFromRaw(raw.Span);
                    hasHatedCharName = true;
                    break;
            }
        }
        return new MonsterCoreAttrs
        {
            CurHp = curHp, MaxHp = maxHp, Level = level,
            HasCurHp = hasCurHp, HasMaxHp = hasMaxHp, HasLevel = hasLevel,
            Name = name, TemplateId = templateId,
            HasName = hasName, HasTemplateId = hasTemplateId,
            BreakingStage = breakingStage, Extinction = extinction, MaxExtinction = maxExtinction,
            Stunned = stunned, MaxStunned = maxStunned, InOverdrive = inOverdrive,
            HasBreakingStage = hasBreakingStage, HasExtinction = hasExtinction, HasMaxExtinction = hasMaxExtinction,
            HasStunned = hasStunned, HasMaxStunned = hasMaxStunned, HasInOverdrive = hasInOverdrive,
            ShieldTotal = shieldTotal, ShieldMaxTotal = shieldMaxTotal, HasShield = hasShield,
            IsLockStunned = isLockStunned, StopBreakingTicking = stopBreakingTicking,
            State = state, DeadType = deadType, DeadTime = deadTime, FirstAttack = firstAttack,
            HatedCharId = hatedCharId, HatedCharName = hatedCharName,
            HasIsLockStunned = hasIsLockStunned, HasStopBreakingTicking = hasStopBreakingTicking,
            HasState = hasState, HasDeadType = hasDeadType, HasDeadTime = hasDeadTime,
            HasFirstAttack = hasFirstAttack, HasHatedCharId = hasHatedCharId, HasHatedCharName = hasHatedCharName,
        };
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
        var attrUpdates = new List<NearDeltaAttrUpdate>();
        foreach (var d in msg.DeltaInfos)
        {
            uuids.Add(d.Uuid);
            // S111: surface monster core attrs (HP/MaxHp/Level) on the
            // delta path so the bridge can update existing entity-table
            // rows. Mirrors Python's `_process_monster_attr_collection`
            // call from the SyncNearDeltaInfo handler at packet_parser.py
            // 4021. Skip rows with no recognised attrs (all-zero) — they
            // would be no-op writes that just churn the immutable dict.
            if (d.Uuid == 0 || d.Attrs is null) continue;
            var attrs = SyncNearEntitiesDecoder.ExtractMonsterCoreAttrs(d.Attrs);
            if (!attrs.Any) continue;
            attrUpdates.Add(new NearDeltaAttrUpdate(d.Uuid, attrs.CurHp, attrs.MaxHp, attrs.Level)
            {
                HasCurHp = attrs.HasCurHp,
                HasMaxHp = attrs.HasMaxHp,
                HasLevel = attrs.HasLevel,
                Name = attrs.Name,
                TemplateId = attrs.TemplateId,
                HasName = attrs.HasName,
                HasTemplateId = attrs.HasTemplateId,
                Attrs = attrs,
            });
        }
        emit(new NearDeltaEvent(uuids, timestampSeconds) { AttrUpdates = attrUpdates });

        // S125 — BuffInfos / BuffEffect per delta. Mirrors Python's
        // `_on_sync_near_delta` calling `_process_aoi_sync_delta` for
        // each delta at packet_parser.py 4014. Same per-branch logic as
        // the S123/S124 0x2E BaseDelta path; emitted once per delta uuid
        // so the bridge routes each row independently.
        foreach (var d in msg.DeltaInfos)
        {
            if (d is null || d.Uuid == 0) continue;

            // S125a: BuffInfos (sustained-buff resync) — uuid-agnostic at
            // decoder, bridge classifies player vs monster.
            if (d.BuffInfos is not null && d.BuffInfos.BuffInfos is not null
                && d.BuffInfos.BuffInfos.Count > 0)
            {
                var payloads = new List<BuffPayload>();
                foreach (var bi in d.BuffInfos.BuffInfos)
                {
                    if (bi is null) continue;
                    if (bi.BaseId == 0) continue;
                    payloads.Add(new BuffPayload(
                        Id: bi.BaseId,
                        Uuid: bi.BuffUuid,
                        BeginMs: bi.CreateTime,
                        DurationMs: bi.Duration,
                        Layer: bi.Layer,
                        Count: bi.Count,
                        Name: string.Empty));
                }
                if (payloads.Count > 0)
                {
                    emit(new AoiBuffSyncEvent(d.Uuid, payloads, timestampSeconds));
                }
            }

            // S125b: BuffEffect (boss one-shot triggers) — gated to
            // non-player low-marker (matches Python's `target_is_monster`).
            if (!CyCombat.IsPlayerUuid((ulong)d.Uuid)
                && d.BuffEffect is not null
                && d.BuffEffect.BuffEffects is not null
                && d.BuffEffect.BuffEffects.Count > 0)
            {
                var effects = new List<BuffEffectPayload>();
                foreach (var be in d.BuffEffect.BuffEffects)
                {
                    if (be is null) continue;
                    var type = (int)be.Type;
                    if (!BuffEventType.BossEvents.Contains(type)) continue;
                    var effectiveHost = be.HostUuid != 0 ? be.HostUuid : d.Uuid;
                    effects.Add(new BuffEffectPayload(type, be.BuffUuid, effectiveHost));
                }
                if (effects.Count > 0)
                {
                    emit(new BuffEffectEvent(d.Uuid, effects, timestampSeconds));
                }
            }

            // S127 — SkillEffect per delta. Mirrors Python's
            // `_on_sync_near_delta` calling `_process_aoi_sync_delta`
            // per delta (packet_parser.py 4014) which dispatches the
            // SkillEffects branch at 4073–4075. Same per-row decode/
            // filter as the 0x2E BaseDelta path; uuid-agnostic at the
            // decoder so DPS attribution can route per row.
            if (d.SkillEffects is not null)
            {
                var ev = SyncToMeDeltaInfoDecoder.ExtractSkillEffect(
                    d.Uuid, d.SkillEffects, timestampSeconds);
                if (ev is not null) emit(ev);
            }
        }
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
        // Resolve uuid: prefer DeltaInfo.Uuid (field 5); fall back to
        // BaseDelta.Uuid only when the primary is 0 AND BaseDelta is
        // present AND its uuid is a player-low-marker (640) — mirrors
        // Python's _on_sync_to_me_delta lines 3893–3902. The BaseDelta
        // path exists because SyncToMeDelta packets are always about
        // self; some early frames after scene switch arrive with
        // primary uuid cleared but BaseDelta carrying the correct
        // player uuid.
        long uuid = d.Uuid;
        if (uuid == 0 && d.BaseDelta is not null)
        {
            var baseUuid = d.BaseDelta.Uuid;
            if (baseUuid != 0 && CyCombat.IsPlayerUuid((ulong)baseUuid))
            {
                uuid = baseUuid;
            }
        }
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
        emit(new ToMeDeltaEvent(uuid, hateCount, cds, timestampSeconds));

        // Sibling event for FightResCDs (resource cooldowns). Mirrors
        // Python's `if res_id > 0` filter at packet_parser.py 3964.
        // Skipped when no qualifying rows so consumers don't see empty
        // events on every sync tick.
        if (d.FightResCDs is not null && d.FightResCDs.Count > 0)
        {
            var fcds = new List<FightResCdSnapshot>();
            foreach (var fcd in d.FightResCDs)
            {
                if (fcd is null) continue;
                if (fcd.ResId <= 0) continue;
                fcds.Add(new FightResCdSnapshot(
                    ResId: fcd.ResId,
                    BeginMs: fcd.BeginTime,
                    DurationMs: fcd.Duration,
                    ValidCdTimeMs: fcd.ValidCDTime));
            }
            if (fcds.Count > 0)
            {
                emit(new ToMeFightResCdEvent(uuid, fcds, timestampSeconds));
            }
        }

        // S121 — AoiSyncDelta on BaseDelta. Mirrors Python's
        // `if di.HasField('BaseDelta'): self._process_aoi_sync_delta(di.BaseDelta)`
        // at packet_parser.py:3979–3981. BaseDelta carries the AOI delta
        // for surrounding entities riding the SyncToMeDeltaInfo packet
        // path. When the BaseDelta uuid is a non-player (monster low
        // marker mismatch), surface its core attrs as a sibling
        // NearDeltaEvent so the bridge's ApplyNearDelta can mirror
        // HP / MaxHp / break-gauge / shield / aggro into an existing
        // MonsterData row. Player-uuid BaseDeltas belong to the
        // (still-unported) _process_attr_collection path — skipped here
        // to avoid writing self into the monster table.
        var baseD = d.BaseDelta;
        if (baseD is not null && baseD.Uuid != 0
            && !CyCombat.IsPlayerUuid((ulong)baseD.Uuid)
            && baseD.Attrs is not null)
        {
            var bAttrs = SyncNearEntitiesDecoder.ExtractMonsterCoreAttrs(baseD.Attrs);
            if (bAttrs.Any)
            {
                var update = new NearDeltaAttrUpdate(baseD.Uuid, bAttrs.CurHp, bAttrs.MaxHp, bAttrs.Level)
                {
                    HasCurHp = bAttrs.HasCurHp,
                    HasMaxHp = bAttrs.HasMaxHp,
                    HasLevel = bAttrs.HasLevel,
                    Name = bAttrs.Name,
                    TemplateId = bAttrs.TemplateId,
                    HasName = bAttrs.HasName,
                    HasTemplateId = bAttrs.HasTemplateId,
                    Attrs = bAttrs,
                };
                emit(new NearDeltaEvent(new[] { baseD.Uuid }, timestampSeconds)
                {
                    AttrUpdates = new[] { update },
                });
            }
        }

        // S122 — TempAttrs on BaseDelta. Mirrors Python's
        // `if delta.HasField('TempAttrs') and target_is_player:` branch
        // at packet_parser.py 4024–4026. Sums TempAttr ids 100/101/103
        // (cd_pct, cd_fixed, cd_accel) across the collection and emits
        // a TempAttrCdEvent. Gated on player low-marker — matches the
        // `target_is_player` predicate. The bridge applies a SelfUuid
        // filter (mirroring `_is_confirmed_self_uid`) so cross-player
        // packets don't write into the local CDR fields.
        if (baseD is not null && baseD.Uuid != 0
            && CyCombat.IsPlayerUuid((ulong)baseD.Uuid)
            && baseD.TempAttrs is not null)
        {
            int cdPct = 0, cdFixed = 0, cdAccel = 0;
            if (baseD.TempAttrs.Attrs is not null)
            {
                foreach (var ta in baseD.TempAttrs.Attrs)
                {
                    if (ta is null) continue;
                    switch (ta.Id)
                    {
                        case 100: cdPct += ta.Value; break;
                        case 101: cdFixed += ta.Value; break;
                        case 103: cdAccel += ta.Value; break;
                    }
                }
            }
            emit(new TempAttrCdEvent(baseD.Uuid, cdPct, cdFixed, cdAccel, timestampSeconds));
        }

        // S123 — BuffInfos on BaseDelta (field 10). Mirrors Python's
        // `_decode_buff_info_sync_pb` + AoiSyncDelta dispatch at
        // packet_parser.py 4032–4041. BuffInfoSync wraps
        // `repeated BuffInfo BuffInfos`; rows with BaseId == 0 are
        // skipped (matches Python's `if not base_id: continue`). When
        // no valid rows survive the filter the event is suppressed —
        // symmetric with Python's `if buffs:` guard. Player vs monster
        // routing is deferred to the bridge (player low marker → write
        // SelfBuffs; non-player → write MonsterDataMap[uuid].BuffList).
        if (baseD is not null && baseD.Uuid != 0
            && baseD.BuffInfos is not null
            && baseD.BuffInfos.BuffInfos is not null
            && baseD.BuffInfos.BuffInfos.Count > 0)
        {
            var payloads = new List<BuffPayload>();
            foreach (var bi in baseD.BuffInfos.BuffInfos)
            {
                if (bi is null) continue;
                if (bi.BaseId == 0) continue;
                payloads.Add(new BuffPayload(
                    Id: bi.BaseId,
                    Uuid: bi.BuffUuid,
                    BeginMs: bi.CreateTime,
                    DurationMs: bi.Duration,
                    Layer: bi.Layer,
                    Count: bi.Count,
                    Name: string.Empty));
            }
            if (payloads.Count > 0)
            {
                emit(new AoiBuffSyncEvent(baseD.Uuid, payloads, timestampSeconds));
            }
        }

        // S124 — BuffEffect on BaseDelta (field 11). Mirrors Python's
        // `if delta.HasField('BuffEffect') and target_is_monster:
        //     self._process_buff_effect_sync(uuid, delta.BuffEffect)`
        // at packet_parser.py 4029–4030 + `_process_buff_effect_sync`
        // at 4789. Filters rows by the boss-event allow-list (HostDeath,
        // ShieldBroken, EnterBreaking, etc — see BuffEventType). Skips
        // emit when no rows survive (parity with Python's per-row
        // `if event_type not in _BOSS_BUFF_EVENTS: continue`). Gated to
        // non-player low-marker so player-side buff effects (a path
        // Python doesn't dispatch here) don't surface as boss events.
        if (baseD is not null && baseD.Uuid != 0
            && !CyCombat.IsPlayerUuid((ulong)baseD.Uuid)
            && baseD.BuffEffect is not null
            && baseD.BuffEffect.BuffEffects is not null
            && baseD.BuffEffect.BuffEffects.Count > 0)
        {
            var effects = new List<BuffEffectPayload>();
            foreach (var be in baseD.BuffEffect.BuffEffects)
            {
                if (be is null) continue;
                var type = (int)be.Type;
                if (!BuffEventType.BossEvents.Contains(type)) continue;
                // Mirror Python: `buff_host = be.HostUuid if be.HostUuid != 0 else host_uuid`.
                var effectiveHost = be.HostUuid != 0 ? be.HostUuid : baseD.Uuid;
                effects.Add(new BuffEffectPayload(type, be.BuffUuid, effectiveHost));
            }
            if (effects.Count > 0)
            {
                emit(new BuffEffectEvent(baseD.Uuid, effects, timestampSeconds));
            }
        }

        // S126 — player-self AttrCollection on BaseDelta. Mirrors
        // Python's `if di.HasField('BaseDelta') and target_is_player:
        //     self._process_attr_collection(uid, delta.Attrs)` at
        // packet_parser.py 4017–4018. Closes the gap S121 explicitly
        // skipped (it gated the AttrCollection mirror to non-player
        // uuids only). Identity + HP + profession slice — combat-stat
        // fields (Attack/Defense/Crit/etc) deferred to a follow-up
        // session because they need new GameState slots. The bridge
        // applies a SelfUuid filter + S109 cold-start latching so
        // cross-player packets don't write into the local player.
        if (baseD is not null && baseD.Uuid != 0
            && CyCombat.IsPlayerUuid((ulong)baseD.Uuid)
            && baseD.Attrs is not null)
        {
            var ev = ExtractPlayerAttrs(baseD.Uuid, baseD.Attrs, timestampSeconds);
            if (ev is not null) emit(ev);
        }

        // S127 — SkillEffect on BaseDelta (field 7). Mirrors Python's
        // `if delta.HasField('SkillEffects'): self._process_skill_effect(...)`
        // at packet_parser.py 4073–4075. Uuid-agnostic (player + monster
        // both surface — DPS attribution layer routes per-row by
        // attacker/target). Suppressed when no damages survive the
        // MISS/FALL/zero-damage filter.
        if (baseD is not null && baseD.Uuid != 0
            && baseD.SkillEffects is not null)
        {
            var ev = ExtractSkillEffect(baseD.Uuid, baseD.SkillEffects, timestampSeconds);
            if (ev is not null) emit(ev);
        }
    }

    // S126 — player-side AttrCollection extractor. Mirrors the
    // identity + HP + profession branches of Python's
    // `_process_attr_collection` at packet_parser.py 4894–5050.
    // Returns null when no recognised fields are present (avoids
    // emitting empty events on every BaseDelta tick).
    internal static PlayerAttrEvent? ExtractPlayerAttrs(long uuid, AttrCollection attrs, double ts)
    {
        if (attrs.Attrs is null || attrs.Attrs.Count == 0) return null;
        string name = string.Empty;
        int level = 0, rankLevel = 0, fightPoint = 0;
        int hp = 0, maxHp = 0, professionId = 0;
        bool hasName = false, hasLevel = false, hasRankLevel = false;
        bool hasFightPoint = false, hasHp = false, hasMaxHp = false;
        bool hasProfessionId = false;

        foreach (var a in attrs.Attrs)
        {
            var id = a.Id;
            var raw = a.RawData;
            if (id == 0 || raw is null || raw.Length == 0) continue;
            switch (id)
            {
                case 1: // AttrType.NAME
                    name = CyPacketExtras.DecodeStringFromRaw(raw.Span);
                    hasName = true;
                    break;
                case 220: // AttrType.PROFESSION_ID
                    professionId = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasProfessionId = true;
                    break;
                case 10000: // AttrType.LEVEL
                    level = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasLevel = true;
                    break;
                case 10030: // AttrType.FIGHT_POINT
                    fightPoint = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasFightPoint = true;
                    break;
                case 10060: // AttrType.RANK_LEVEL
                    rankLevel = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasRankLevel = true;
                    break;
                case 11310: // AttrType.HP
                    hp = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasHp = true;
                    break;
                case 11320: // AttrType.MAX_HP
                    maxHp = CyPacketExtras.RawVarintToInt32(raw.Span);
                    hasMaxHp = true;
                    break;
            }
        }

        var ev = new PlayerAttrEvent(uuid, ts)
        {
            Name = name, Level = level, RankLevel = rankLevel,
            FightPoint = fightPoint, Hp = hp, MaxHp = maxHp,
            ProfessionId = professionId,
            HasName = hasName, HasLevel = hasLevel, HasRankLevel = hasRankLevel,
            HasFightPoint = hasFightPoint, HasHp = hasHp, HasMaxHp = hasMaxHp,
            HasProfessionId = hasProfessionId,
        };
        return ev.Any ? ev : null;
    }

    // S127 — SkillEffect → SkillEffectEvent extractor. Mirrors the
    // body of Python's `_process_skill_effect` (packet_parser.py
    // 4077–4090) and the per-row decode/filter slice of
    // `_decode_sync_damage_info` (4092–4151). Per Python, MISS/FALL
    // rows are skipped entirely; the canonical damage amount is
    // resolved via CyCombat.CombatDamageAmount; zero-damage rows are
    // dropped UNLESS the row signals IMMUNE/ABSORBED (those carry
    // invincibility events with no numeric damage). Returns null when
    // no rows survive so consumers don't see empty events.
    internal static SkillEffectEvent? ExtractSkillEffect(long targetUuid, SkillEffect se, double ts)
    {
        if (se.Damages is null || se.Damages.Count == 0) return null;
        var rows = new List<DamagePayload>(se.Damages.Count);
        foreach (var dmg in se.Damages)
        {
            if (dmg is null) continue;
            var damageType = (int)dmg.Type;
            // Skip MISS / FALL — Python drops them at the head of
            // `_decode_sync_damage_info` (line 4097–4098).
            if (damageType == (int)EDamageType.Miss
                || damageType == (int)EDamageType.Fall) continue;

            var isHeal = damageType == (int)EDamageType.Heal;
            var isImmune = damageType == (int)EDamageType.Immune;
            var isAbsorbed = damageType == (int)EDamageType.Absorbed;

            var damage = CyCombat.CombatDamageAmount(
                dmg.Value, dmg.LuckyValue, dmg.ActualValue,
                dmg.HpLessenValue, dmg.ShieldLessenValue);
            // Python: `if not (is_immune or is_absorbed): if damage_amount <= 0: return`
            // (lines 4148–4151). Immune / Absorbed events carry zero
            // damage but still need to flow through for invincibility
            // bookkeeping.
            if (damage <= 0 && !(isImmune || isAbsorbed)) continue;

            rows.Add(new DamagePayload(
                AttackerUuid: dmg.AttackerUuid,
                TopSummonerId: dmg.TopSummonerId,
                SkillId: dmg.OwnerId,
                DamageSource: (int)dmg.DamageSource,
                OwnerLevel: dmg.OwnerLevel,
                OwnerStage: dmg.OwnerStage,
                HitEventId: dmg.HitEventId,
                PassiveUuid: dmg.PassiveUuid,
                DamageType: damageType,
                TypeFlag: dmg.TypeFlag,
                DamageMode: (int)dmg.DamageMode,
                Damage: damage,
                HpLessen: dmg.HpLessenValue,
                ShieldLessen: dmg.ShieldLessenValue,
                Element: (int)dmg.Property,
                IsCrit: (dmg.TypeFlag & 1) != 0,
                IsDead: dmg.IsDead,
                IsNormal: dmg.IsNormal,
                IsRainbow: dmg.IsRainbow,
                IsHeal: isHeal,
                IsImmune: isImmune,
                IsAbsorbed: isAbsorbed));
        }
        if (rows.Count == 0) return null;
        return new SkillEffectEvent(targetUuid, rows, ts);
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
