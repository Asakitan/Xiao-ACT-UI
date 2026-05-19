using System.Buffers.Binary;
using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S71 — packet-parity gold scenario. Exercises the full
/// PacketBridge path end-to-end (decode → emit → mutate) for a
/// representative session: enter game → identity sync → server time
/// → skill cooldown → near-entity appear → dirty HP update → near-
/// entity disappear → kick-off. Asserts the resulting GameState
/// snapshot is bit-identical to the expected gold values.
///
/// Why this matters: the per-method decoder + per-event mutator
/// pair count is now 22 + 17. A single regression in any link of
/// the chain (decoder field rename, mutator slot swap, event
/// payload restructure) would only show up under this kind of
/// integration test. Per-component unit tests (Session29-S70)
/// catch local breakage; this catches glue-layer drift.
/// </summary>
public class PacketParityGoldScenarioTests
{
    [Fact]
    public void FullSessionScenario_ProducesExpectedGameState()
    {
        var state = new GameStateManager();
        var registry = MethodDecoderRegistry.BuildDefault();
        var bridge = new PacketBridge(state, new PacketParser(), registry);

        const ulong selfUuid = 0x1234_5678_9ABC_DEF0UL;
        const long selfUuidS = unchecked((long)selfUuid);
        const long charId = 8888L;

        // 1. EnterGame — sets SelfUuid (use Apply since the decoder
        //    expects a CyPacket field stream that's tedious to synth).
        bridge.Apply(new EnterGameEvent(selfUuid, 100.0));

        // 2. SyncServerTime — sets ServerTimeOffsetMs. Use values
        //    near wall-clock so the offset stays bounded; we just
        //    assert it's non-zero (proves the decoder ran end-to-end).
        var nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var serverTime = new SyncServerTime
        {
            ClientMilliseconds = nowMs,
            ServerMilliseconds = nowMs + 500,  // +500ms ahead of client
        };
        bridge.DispatchRawNotify(NotifyMethod.SyncServerTime,
            serverTime.ToByteArray(), nowMs / 1000.0);

        // 3. SyncContainerData — identity + initial HP.
        var sync = new SyncContainerData
        {
            VData = new CharSerialize
            {
                CharId = charId,
                CharBase = new CharBaseInfo { Name = "Asuna", FightPoint = 99000 },
                RoleLevel = new RoleLevel { Level = 75 },
                Attr = new UserFightAttr { CurHp = 5000, MaxHp = 5000, OriginEnergy = 100f },
            },
        };
        bridge.DispatchRawNotify(NotifyMethod.SyncContainerData,
            sync.ToByteArray(), 101.0);

        // 4. SyncToMeDeltaInfo — one active skill cooldown.
        var toMe = new SyncToMeDeltaInfo
        {
            DeltaInfo = new AoiSyncToMeDelta
            {
                Uuid = selfUuidS,
                SyncSkillCDs =
                {
                    new SkillCDInfo
                    {
                        SkillLevelId = 50301,
                        SkillBeginTime = 200_000_000L,
                        Duration = 8_000,
                        ValidCDTimeLegacy = 1_500,
                        ChargeCount = 2,
                    },
                },
            },
        };
        bridge.DispatchRawNotify(NotifyMethod.SyncToMeDeltaInfo,
            toMe.ToByteArray(), 200_000.0);

        // 5. SyncNearEntities — boss appears in radius.
        var near = BuildNearEntitiesAppear(uuid: 99001L, entityType: 7);
        bridge.DispatchRawNotify(NotifyMethod.SyncNearEntities, near, 102.0);

        // 6. SyncContainerDirtyData — HP drops to 1500 (took damage).
        var dirty = BuildDirtyStream(fieldIndex: 16, subField: 1, payload: PayloadU32(1500));
        var dirtyMsg = new SyncContainerDirtyData
        {
            VData = new BufferStream { Buffer = ByteString.CopyFrom(dirty) },
        };
        bridge.DispatchRawNotify(NotifyMethod.SyncContainerDirtyData,
            dirtyMsg.ToByteArray(), 103.0);

        // 7. SyncNearEntities (Disappear) — boss leaves radius.
        var disappear = BuildNearEntitiesDisappear(uuid: 99001L, reason: 0);
        bridge.DispatchRawNotify(NotifyMethod.SyncNearEntities, disappear, 104.0);

        // 8. NotifyClientKickOff — session ends.
        bridge.DispatchRawNotify(NotifyMethod.NotifyClientKickOff,
            Array.Empty<byte>(), 105.0);

        // ── Gold assertions: every slot we touched ──
        var snap = state.Snapshot;
        Assert.Equal(selfUuid, snap.SelfUuid);
        Assert.Equal("Asuna", snap.PlayerName);
        Assert.Equal(75, snap.LevelBase);
        Assert.Equal(99000, snap.FightPoint);
        Assert.Equal(1500, snap.HpCurrent);                     // dirty overwrote
        Assert.Equal(5000, snap.HpMax);
        Assert.Equal(0.30, snap.HpPct, 6);                      // recomputed
        Assert.NotEqual(0.0, snap.ServerTimeOffsetMs);          // decoder ran
        Assert.True(snap.SkillCdMap.ContainsKey(50301));
        Assert.Equal(8_000, snap.SkillCdMap[50301].DurationMs);
        Assert.Equal(2, snap.SkillCdMap[50301].ChargeCount);
        Assert.False(snap.NearEntities.ContainsKey(99001L));    // disappeared
        Assert.False(snap.PacketActive);                        // kicked off
        Assert.Contains("kick", snap.ErrorMsg, StringComparison.OrdinalIgnoreCase);

        // EventsApplied must have ticked at least once per non-no-op step
        // (server-time, container-sync, to-me, near-appear, dirty-hp,
        //  near-disappear, kick-off; EnterGame uses Apply directly so
        //  that bumps as well — at minimum 8 mutations).
        Assert.True(bridge.EventsApplied >= 8,
            $"expected >=8 mutations, got {bridge.EventsApplied}");
    }

    // ── helpers ─────────────────────────────────────────────────

    private static byte[] BuildDirtyStream(int fieldIndex, int subField, byte[] payload)
    {
        var buf = new byte[24 + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(0, 4), 0xFFFFFFFEu);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(8, 4), (uint)fieldIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(12, 4), 0xFFFFFFFEu);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(20, 4), (uint)subField);
        Buffer.BlockCopy(payload, 0, buf, 24, payload.Length);
        return buf;
    }

    private static byte[] PayloadU32(uint v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, v);
        return b;
    }

    private static byte[] BuildNearEntitiesAppear(long uuid, int entityType)
    {
        var msg = new SyncNearEntities
        {
            Appear = { new Entity { Uuid = uuid, EntType = (EEntityType)entityType } },
        };
        return msg.ToByteArray();
    }

    private static byte[] BuildNearEntitiesDisappear(long uuid, int reason)
    {
        var msg = new SyncNearEntities
        {
            Disappear = { new DisappearEntity { Uuid = uuid, Type = (EDisappearType)reason } },
        };
        return msg.ToByteArray();
    }
}
