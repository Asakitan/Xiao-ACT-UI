using System.Collections.Generic;

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
/// Mirror of <c>packet_parser.NotifyMethod</c> ids (packet_parser.py:201-292).
/// We surface the full id table — even ids the C# port does not yet decode —
/// so <see cref="PacketBridge.DispatchRawNotify"/> can name unhandled notifies
/// in the log. Decoders are registered against a subset of these ids via
/// <see cref="MethodDecoderRegistry"/>; the rest fall through to a named
/// "unhandled" debug line.
///
/// <para>Several ids exceed <see cref="ushort"/> range (the 0x29000+ shop /
/// world-boss / match / payment ranges) so all constants are typed as
/// <see langword="int"/>.</para>
/// </summary>
public static class NotifyMethod
{
    /// <summary>
    /// PROTO-01: the canonical buff id the server fires when the party wipes
    /// and the next damage should open a fresh encounter (mirrors Python's
    /// <c>WIPE_BUFF_BASE_ID = 510072</c> at packet_parser.py:1756). Reachable
    /// from <see cref="NotifyMethod.NotifyBuffChange"/> in the legacy path
    /// and from buff snapshots reaching the local self uuid.
    /// </summary>
    public const long WipeBuffBaseId = 510072L;

    // ── Core sync (handled with full logic) ──
    public const int EnterScene = 0x03;
    public const int SyncNearEntities = 0x06;
    public const int SyncContainerData = 0x15;
    public const int SyncContainerDirtyData = 0x16;
    public const int SyncServerTime = 0x2B;
    public const int SyncNearDeltaInfo = 0x2D;
    public const int SyncToMeDeltaInfo = 0x2E;
    // ── Login / Session ──
    public const int SyncPioneerInfo = 0x0E;
    public const int SyncSwitchChange = 0x12;
    public const int SyncSwitchInfo = 0x13;
    public const int EnterGame = 0x14;
    public const int SyncDungeonData = 0x17;
    public const int SyncDungeonDirtyData = 0x18;
    // ── Awards / Items ──
    public const int AwardNotify = 0x19;
    public const int CardInfoAck = 0x1A;
    public const int SyncSeason = 0x1B;
    // ── Actions / Social ──
    public const int UserAction = 0x1C;
    public const int NotifyDisplayPlayHelp = 0x1D;
    public const int NotifyApplicationInteraction = 0x1E;
    public const int NotifyIsAgree = 0x1F;
    public const int NotifyCancelAction = 0x20;
    public const int NotifyUploadPictureResult = 0x21;
    public const int SyncInvite = 0x24;
    public const int NotifyRedDotChange = 0x25;
    public const int ChangeNameResultNtf = 0x26;
    // ── Combat / Revive ──
    public const int NotifyReviveUser = 0x27;
    // ── Parkour ──
    public const int NotifyParkourRankInfo = 0x28;
    public const int NotifyParkourRecordInfo = 0x29;
    // ── UI Notifications ──
    public const int NotifyShowTips = 0x2A;
    public const int NotifyNoticeInfo = 0x2C;
    // ── Session management ──
    public const int NotifyClientKickOff = 0x31;
    public const int PaymentResponse = 0x33;
    public const int NotifyUnlockCookBook = 0x35;
    public const int NotifyCustomEvent = 0x36;
    public const int NotifyStartPlayingDungeon = 0x37;
    public const int ChangeShowIdResultNtf = 0x38;
    public const int NotifyShowItems = 0x39;
    public const int NotifySeasonActivationTargetInfo = 0x3A;
    public const int NotifyTextCheckResult = 0x3B;
    public const int NotifyDebugMessageTip = 0x3D;
    public const int NotifyUserCloseFunction = 0x3E;
    public const int NotifyServerCloseFunction = 0x3F;
    public const int SyncClientUseSkillWorld = 0x43;
    // ── Team ──
    public const int NotifyAwardAllItems = 0x45;
    public const int NotifyAllMemberReady = 0x46;
    public const int NotifyCaptainReady = 0x47;
    // ── Privilege / Quest / BattlePass ──
    public const int NotifyUserAllSourcePrivilegeEffectData = 0x4A;
    public const int NotifyQuestAccept = 0x4B;
    public const int NotifyQuestChangeStep = 0x4C;
    public const int NotifyQuestGiveUp = 0x4D;
    public const int NotifyQuestComplete = 0x4E;
    public const int NotifyUserAllValidBattlePassData = 0x4F;
    public const int NotifyNoticeMultiLanguageInfo = 0x53;
    // ── Skill / Combat (battle server 0x3000 range) ──
    public const int QteBegin = 0x3001;                  // 12289
    public const int SyncClientUseSkill = 0x3002;        // 12290
    public const int NotifyBuffChange = 0x3003;          // 12291
    public const int SyncServerSkillStageEnd = 0x3004;   // 12292
    public const int SyncServerSkillEnd = 0x3005;        // 12293
    // ── Quest (0x6000 range) ──
    public const int QuestAbort = 0x6001;
    // ── Shop (0x29000 range) ──
    public const int NotifyBuyShopResult = 0x29001;
    public const int NotifyShopItemCanBuy = 0x29002;
    // ── World Boss (0x46000 range) ──
    public const int WorldBossRankInfoNtf = 0x46001;
    // ── Match (0x48000 range) ──
    public const int EnterMatchResultNtf = 0x48001;
    // ── Ride (0x4D000 range) ──
    public const int NotifyDriverApplyRide = 0x4D001;
    public const int NotifyInviteApplyRide = 0x4D002;
    public const int NotifyRideIsAgree = 0x4D003;
    // ── Payment (0x51000 range) ──
    public const int NotifyPayInfo = 0x51001;
    // ── Life Profession (0x52000 range) ──
    public const int NotifyLifeProfessionWorkHistoryChange = 0x52001;
    public const int NotifyLifeProfessionUnlockRecipe = 0x52002;
    // ── Sign-in (0x5E000 range) ──
    public const int SignRewardNotify = 0x5E001;
    // ── Random (0x6B000 range) ──
    public const int NotifyEntryRandomData = 0x6B001;

    /// <summary>
    /// Hard-coded id→name reverse lookup, mirroring Python's
    /// <c>_NOTIFY_METHOD_NAMES</c> at packet_parser.py:295-299. Used by
    /// <c>PacketBridge.DispatchRawNotify</c> to log unhandled-but-known
    /// notifies by name (instead of just a bare hex id), so we can debug
    /// unported decoders without recompiling.
    /// </summary>
    public static readonly IReadOnlyDictionary<int, string> Names = new Dictionary<int, string>
    {
        // Core sync
        { EnterScene, "ENTER_SCENE" },
        { SyncNearEntities, "SYNC_NEAR_ENTITIES" },
        { SyncContainerData, "SYNC_CONTAINER_DATA" },
        { SyncContainerDirtyData, "SYNC_CONTAINER_DIRTY_DATA" },
        { SyncServerTime, "SYNC_SERVER_TIME" },
        { SyncNearDeltaInfo, "SYNC_NEAR_DELTA_INFO" },
        { SyncToMeDeltaInfo, "SYNC_TO_ME_DELTA_INFO" },
        // Login / Session
        { SyncPioneerInfo, "SYNC_PIONEER_INFO" },
        { SyncSwitchChange, "SYNC_SWITCH_CHANGE" },
        { SyncSwitchInfo, "SYNC_SWITCH_INFO" },
        { EnterGame, "ENTER_GAME" },
        { SyncDungeonData, "SYNC_DUNGEON_DATA" },
        { SyncDungeonDirtyData, "SYNC_DUNGEON_DIRTY_DATA" },
        // Awards / Items
        { AwardNotify, "AWARD_NOTIFY" },
        { CardInfoAck, "CARD_INFO_ACK" },
        { SyncSeason, "SYNC_SEASON" },
        // Actions / Social
        { UserAction, "USER_ACTION" },
        { NotifyDisplayPlayHelp, "NOTIFY_DISPLAY_PLAY_HELP" },
        { NotifyApplicationInteraction, "NOTIFY_APPLICATION_INTERACTION" },
        { NotifyIsAgree, "NOTIFY_IS_AGREE" },
        { NotifyCancelAction, "NOTIFY_CANCEL_ACTION" },
        { NotifyUploadPictureResult, "NOTIFY_UPLOAD_PICTURE_RESULT" },
        { SyncInvite, "SYNC_INVITE" },
        { NotifyRedDotChange, "NOTIFY_RED_DOT_CHANGE" },
        { ChangeNameResultNtf, "CHANGE_NAME_RESULT_NTF" },
        // Combat / Revive
        { NotifyReviveUser, "NOTIFY_REVIVE_USER" },
        // Parkour
        { NotifyParkourRankInfo, "NOTIFY_PARKOUR_RANK_INFO" },
        { NotifyParkourRecordInfo, "NOTIFY_PARKOUR_RECORD_INFO" },
        // UI Notifications
        { NotifyShowTips, "NOTIFY_SHOW_TIPS" },
        { NotifyNoticeInfo, "NOTIFY_NOTICE_INFO" },
        // Session management
        { NotifyClientKickOff, "NOTIFY_CLIENT_KICK_OFF" },
        { PaymentResponse, "PAYMENT_RESPONSE" },
        { NotifyUnlockCookBook, "NOTIFY_UNLOCK_COOK_BOOK" },
        { NotifyCustomEvent, "NOTIFY_CUSTOM_EVENT" },
        { NotifyStartPlayingDungeon, "NOTIFY_START_PLAYING_DUNGEON" },
        { ChangeShowIdResultNtf, "CHANGE_SHOW_ID_RESULT_NTF" },
        { NotifyShowItems, "NOTIFY_SHOW_ITEMS" },
        { NotifySeasonActivationTargetInfo, "NOTIFY_SEASON_ACTIVATION_TARGET_INFO" },
        { NotifyTextCheckResult, "NOTIFY_TEXT_CHECK_RESULT" },
        { NotifyDebugMessageTip, "NOTIFY_DEBUG_MESSAGE_TIP" },
        { NotifyUserCloseFunction, "NOTIFY_USER_CLOSE_FUNCTION" },
        { NotifyServerCloseFunction, "NOTIFY_SERVER_CLOSE_FUNCTION" },
        { SyncClientUseSkillWorld, "SYNC_CLIENT_USE_SKILL_WORLD" },
        // Team
        { NotifyAwardAllItems, "NOTIFY_AWARD_ALL_ITEMS" },
        { NotifyAllMemberReady, "NOTIFY_ALL_MEMBER_READY" },
        { NotifyCaptainReady, "NOTIFY_CAPTAIN_READY" },
        // Privilege / Quest / BattlePass
        { NotifyUserAllSourcePrivilegeEffectData, "NOTIFY_USER_ALL_SOURCE_PRIVILEGE_EFFECT_DATA" },
        { NotifyQuestAccept, "NOTIFY_QUEST_ACCEPT" },
        { NotifyQuestChangeStep, "NOTIFY_QUEST_CHANGE_STEP" },
        { NotifyQuestGiveUp, "NOTIFY_QUEST_GIVE_UP" },
        { NotifyQuestComplete, "NOTIFY_QUEST_COMPLETE" },
        { NotifyUserAllValidBattlePassData, "NOTIFY_USER_ALL_VALID_BATTLE_PASS_DATA" },
        { NotifyNoticeMultiLanguageInfo, "NOTIFY_NOTICE_MULTI_LANGUAGE_INFO" },
        // Skill / Combat (battle server 0x3000 range)
        { QteBegin, "QTE_BEGIN" },
        { SyncClientUseSkill, "SYNC_CLIENT_USE_SKILL" },
        { NotifyBuffChange, "NOTIFY_BUFF_CHANGE" },
        { SyncServerSkillStageEnd, "SYNC_SERVER_SKILL_STAGE_END" },
        { SyncServerSkillEnd, "SYNC_SERVER_SKILL_END" },
        // Quest (0x6000 range)
        { QuestAbort, "QUEST_ABORT" },
        // Shop (0x29000 range)
        { NotifyBuyShopResult, "NOTIFY_BUY_SHOP_RESULT" },
        { NotifyShopItemCanBuy, "NOTIFY_SHOP_ITEM_CAN_BUY" },
        // World Boss (0x46000 range)
        { WorldBossRankInfoNtf, "WORLD_BOSS_RANK_INFO_NTF" },
        // Match (0x48000 range)
        { EnterMatchResultNtf, "ENTER_MATCH_RESULT_NTF" },
        // Ride (0x4D000 range)
        { NotifyDriverApplyRide, "NOTIFY_DRIVER_APPLY_RIDE" },
        { NotifyInviteApplyRide, "NOTIFY_INVITE_APPLY_RIDE" },
        { NotifyRideIsAgree, "NOTIFY_RIDE_IS_AGREE" },
        // Payment (0x51000 range)
        { NotifyPayInfo, "NOTIFY_PAY_INFO" },
        // Life Profession (0x52000 range)
        { NotifyLifeProfessionWorkHistoryChange, "NOTIFY_LIFE_PROFESSION_WORK_HISTORY_CHANGE" },
        { NotifyLifeProfessionUnlockRecipe, "NOTIFY_LIFE_PROFESSION_UNLOCK_RECIPE" },
        // Sign-in (0x5E000 range)
        { SignRewardNotify, "SIGN_REWARD_NOTIFY" },
        // Random (0x6B000 range)
        { NotifyEntryRandomData, "NOTIFY_ENTRY_RANDOM_DATA" },
    };
}
